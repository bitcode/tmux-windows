/* $OpenBSD$ */

/*
 * Copyright (c) 2007 Nicholas Marriott <nicholas.marriott@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/utsname.h>

#include <errno.h>
#include <fcntl.h>
#include <langinfo.h>
#include <locale.h>
#include <pwd.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "tmux.h"

struct options	*global_options;	/* server options */
struct options	*global_s_options;	/* session options */
struct options	*global_w_options;	/* window options */
struct environ	*global_environ;

struct timeval	 start_time;
const char	*socket_path;
int		 ptm_fd = -1;
const char	*shell_command;

static __dead void	 usage(int);
static char		*make_label(const char *, char **);

static int		 areshell(const char *);
static const char	*getshell(void);

static __dead void
usage(int status)
{
	fprintf(status ? stderr : stdout,
	    "usage: %s [-2CDhlNuVv] [-c shell-command] [-f file] [-L socket-name]\n"
	    "            [-S socket-path] [-T features] [command [flags]]\n",
	    getprogname());
	exit(status);
}

static const char *
getshell(void)
{
	struct passwd	*pw;
	const char	*shell;

	shell = getenv("SHELL");
	if (checkshell(shell))
		return (shell);

	pw = getpwuid(getuid());
	if (pw != NULL && checkshell(pw->pw_shell))
		return (pw->pw_shell);

	return (_PATH_BSHELL);
}

int
checkshell(const char *shell)
{
	if (shell == NULL || shell[0] == '\0')
		return (0);
#ifdef PLATFORM_WINDOWS
	/* On Windows, executables don't start with '/'; accept any non-empty path */
	if (strcmp(shell, "/bin/sh") == 0 || strcmp(shell, "/bin/csh") == 0 ||
	    strcmp(shell, "/usr/bin/false") == 0)
		return (0);
	return (1);
#else
	if (*shell != '/')
		return (0);
	if (areshell(shell))
		return (0);
	if (access(shell, X_OK) != 0)
		return (0);
	return (1);
#endif
}

static int
areshell(const char *shell)
{
	const char	*progname, *ptr;

	if ((ptr = strrchr(shell, '/')) != NULL)
		ptr++;
	else
		ptr = shell;
	progname = getprogname();
	if (*progname == '-')
		progname++;
	if (strcmp(ptr, progname) == 0)
		return (1);
	return (0);
}

static char *
expand_path(const char *path, const char *home)
{
	char			*expanded, *name;
	const char		*end;
	struct environ_entry	*value;

	if (strncmp(path, "~/", 2) == 0) {
		if (home == NULL)
			return (NULL);
		xasprintf(&expanded, "%s%s", home, path + 1);
		return (expanded);
	}

	if (*path == '$') {
		end = strchr(path, '/');
		if (end == NULL)
			name = xstrdup(path + 1);
		else
			name = xstrndup(path + 1, end - path - 1);
		value = environ_find(global_environ, name);
		free(name);
		if (value == NULL)
			return (NULL);
		if (end == NULL)
			end = "";
		xasprintf(&expanded, "%s%s", value->value, end);
		return (expanded);
	}

	return (xstrdup(path));
}

static void
expand_paths(const char *s, char ***paths, u_int *n, int no_realpath)
{
	const char	*home = find_home();
	char		*copy, *next, *tmp, resolved[PATH_MAX], *expanded;
	char		*path;
	u_int		 i;

	*paths = NULL;
	*n = 0;

	copy = tmp = xstrdup(s);
	while ((next = strsep(&tmp, ":")) != NULL) {
		expanded = expand_path(next, home);
		if (expanded == NULL) {
			log_debug("%s: invalid path: %s", __func__, next);
			continue;
		}
		if (no_realpath)
			path = expanded;
		else {
			if (realpath(expanded, resolved) == NULL) {
				log_debug("%s: realpath(\"%s\") failed: %s", __func__,
			  expanded, strerror(errno));
				free(expanded);
				continue;
			}
			path = xstrdup(resolved);
			free(expanded);
		}
		for (i = 0; i < *n; i++) {
			if (strcmp(path, (*paths)[i]) == 0)
				break;
		}
		if (i != *n) {
			log_debug("%s: duplicate path: %s", __func__, path);
			free(path);
			continue;
		}
		*paths = xreallocarray(*paths, (*n) + 1, sizeof *paths);
		(*paths)[(*n)++] = path;
	}
	free(copy);
}

static char *
make_label(const char *label, char **cause)
{
	char		**paths, *path, *base;
	u_int		  i, n;
	struct stat	  sb;
	uid_t		  uid;

	*cause = NULL;
	if (label == NULL)
		label = "default";
	uid = getuid();

#ifdef PLATFORM_WINDOWS
	const char *localappdata = getenv("LOCALAPPDATA");
	if (localappdata == NULL)
		localappdata = "C:\\Users\\Default\\AppData\\Local";
	/* ensure parent %LOCALAPPDATA%\tmux exists before creating tmux-<uid> */
	xasprintf(&path, "%s\\tmux", localappdata);
	mkdir(path, S_IRWXU);
	free(path);
	xasprintf(&base, "%s\\tmux\\tmux-%ld", localappdata, (long)uid);
#else
	expand_paths(TMUX_SOCK, &paths, &n, 0);
	if (n == 0) {
		xasprintf(cause, "no suitable socket path");
		return (NULL);
	}
	path = paths[0]; /* can only have one socket! */
	for (i = 1; i < n; i++)
		free(paths[i]);
	free(paths);

	xasprintf(&base, "%s/tmux-%ld", path, (long)uid);
	free(path);
#endif
	if (mkdir(base, S_IRWXU) != 0 && errno != EEXIST) {
		xasprintf(cause, "couldn't create directory %s (%s)", base,
		    strerror(errno));
		goto fail;
	}
	if (lstat(base, &sb) != 0) {
		xasprintf(cause, "couldn't read directory %s (%s)", base,
		    strerror(errno));
		goto fail;
	}
	if (!S_ISDIR(sb.st_mode)) {
		xasprintf(cause, "%s is not a directory", base);
		goto fail;
	}
#ifndef PLATFORM_WINDOWS
	if (sb.st_uid != uid || (sb.st_mode & TMUX_SOCK_PERM) != 0) {
		xasprintf(cause, "directory %s has unsafe permissions", base);
		goto fail;
	}
#endif
#ifdef PLATFORM_WINDOWS
	xasprintf(&path, "%s\\%s", base, label);
#else
	xasprintf(&path, "%s/%s", base, label);
#endif
	free(base);
	return (path);

fail:
	free(base);
	return (NULL);
}

char *
shell_argv0(const char *shell, int is_login)
{
	const char	*slash, *name;
	char		*argv0;

	slash = strrchr(shell, '/');
	if (slash != NULL && slash[1] != '\0')
		name = slash + 1;
	else
		name = shell;
	if (is_login)
		xasprintf(&argv0, "-%s", name);
	else
		xasprintf(&argv0, "%s", name);
	return (argv0);
}

void
setblocking(int fd, int state)
{
	int mode;

	if ((mode = fcntl(fd, F_GETFL)) != -1) {
		if (!state)
			mode |= O_NONBLOCK;
		else
			mode &= ~O_NONBLOCK;
		fcntl(fd, F_SETFL, mode);
	}
}

uint64_t
get_timer(void)
{
	struct timespec	ts;

	/*
	 * We want a timestamp in milliseconds suitable for time measurement,
	 * so prefer the monotonic clock.
	 */
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		clock_gettime(CLOCK_REALTIME, &ts);
	return ((ts.tv_sec * 1000ULL) + (ts.tv_nsec / 1000000ULL));
}

const char *
sig2name(int signo)
{
     static char	s[11];

#ifdef HAVE_SYS_SIGNAME
     if (signo > 0 && signo < NSIG)
	     return (sys_signame[signo]);
#endif
     xsnprintf(s, sizeof s, "%d", signo);
     return (s);
}

const char *
find_cwd(void)
{
	char		 resolved1[PATH_MAX], resolved2[PATH_MAX];
	static char	 cwd[PATH_MAX];
	const char	*pwd;

	if (getcwd(cwd, sizeof cwd) == NULL)
		return (NULL);
	if ((pwd = getenv("PWD")) == NULL || *pwd == '\0')
		return (cwd);

	/*
	 * We want to use PWD so that symbolic links are maintained,
	 * but only if it matches the actual working directory.
	 */
	if (realpath(pwd, resolved1) == NULL)
		return (cwd);
	if (realpath(cwd, resolved2) == NULL)
		return (cwd);
	if (strcmp(resolved1, resolved2) != 0)
		return (cwd);
	return (pwd);
}

const char *
find_home(void)
{
	struct passwd		*pw;
	static const char	*home;

	if (home != NULL)
		return (home);

	home = getenv("HOME");
	if (home == NULL || *home == '\0') {
		pw = getpwuid(getuid());
		if (pw != NULL)
			home = pw->pw_dir;
		else
			home = NULL;
	}

	return (home);
}

const char *
getversion(void)
{
	return (TMUX_VERSION);
}

int
main(int argc, char **argv)
{
	char					*path = NULL, *label = NULL;
	char					*cause, **var;
	const char				*s, *cwd;
	int					 opt, keys, feat = 0, fflag = 0;
	uint64_t				 flags = 0;
	const struct options_table_entry	*oe;
	u_int					 i;
#ifdef PLATFORM_WINDOWS
    FILE *fdbg;
#endif

#ifdef PLATFORM_WINDOWS
    win32_log("tmux main start\n");
    if (win32_socket_init() != 0) {
        win32_log("socket_init failed\n");
		errx(1, "win32_socket_init failed");
    }
    win32_log("socket_init done\n");
#endif

	if (setlocale(LC_CTYPE, "en_US.UTF-8") == NULL &&
	    setlocale(LC_CTYPE, "C.UTF-8") == NULL) {
#ifdef PLATFORM_WINDOWS
        win32_log("setlocale failed, trying empty\n");
#endif
		if (setlocale(LC_CTYPE, "") == NULL)
			errx(1, "invalid LC_ALL, LC_CTYPE or LANG");
		s = nl_langinfo(CODESET);
		if (strcasecmp(s, "UTF-8") != 0 && strcasecmp(s, "UTF8") != 0)
			errx(1, "need UTF-8 locale (LC_CTYPE) but have %s", s);
	}
#ifdef PLATFORM_WINDOWS
    win32_log("setlocale done\n");
#endif

	setlocale(LC_TIME, "");
	tzset();

	if (**argv == '-')
		flags = CLIENT_LOGIN;

#ifdef PLATFORM_WINDOWS
    win32_log("Calling environ_create\n");
#endif
	global_environ = environ_create();
#ifdef PLATFORM_WINDOWS
    win32_log("environ_create done\n");
#endif
	for (var = environ; *var != NULL; var++)
		environ_put(global_environ, *var, 0);
#ifdef PLATFORM_WINDOWS
    win32_log("environ populated\n");
#endif
	if ((cwd = find_cwd()) != NULL)
		environ_set(global_environ, "PWD", 0, "%s", cwd);
#ifdef PLATFORM_WINDOWS
    win32_log("PWD set\n");
#endif
#ifdef PLATFORM_WINDOWS
    /*
     * Auto-detect the launching shell so panes inherit it.
     * On Unix, $SHELL is always set.  On Windows it usually isn't,
     * so query the parent process executable and set SHELL if needed.
     * This flows through getshell() → default-shell → spawn_pane().
     */
    if (getenv("SHELL") == NULL) {
        const char *parent = win32_get_parent_shell();
        if (parent != NULL) {
            win32_log("auto-detected parent shell: %s\n", parent);
            setenv("SHELL", parent, 0);
            environ_set(global_environ, "SHELL", 0, "%s", parent);
        }
    }
#endif
#ifdef PLATFORM_WINDOWS
    {
        /* On Windows use %APPDATA%\tmux\tmux.conf as the default config path.
         * expand_paths with no_realpath=1 keeps non-existent paths so the
         * server will attempt to load the file when it does exist. */
        const char *appdata = getenv("APPDATA");
        if (appdata != NULL && appdata[0] != '\0') {
            char win_conf[MAX_PATH];
            snprintf(win_conf, sizeof(win_conf), "%s\\tmux\\tmux.conf", appdata);
            cfg_files = xreallocarray(cfg_files, cfg_nfiles + 1, sizeof *cfg_files);
            cfg_files[cfg_nfiles++] = xstrdup(win_conf);
        }
    }
    win32_log("expand_paths (Windows) done\n");
#else
	expand_paths(TMUX_CONF, &cfg_files, &cfg_nfiles, 1);
#endif
#ifdef PLATFORM_WINDOWS
    win32_log("expand_paths done\n");
#endif

	while ((opt = getopt(argc, argv, "2c:CDdf:hlL:NqS:T:uUvV")) != -1) {
		switch (opt) {
		case '2':
			tty_add_features(&feat, "256", ":,");
			break;
		case 'c':
			shell_command = optarg;
			break;
		case 'D':
			flags |= CLIENT_NOFORK;
			break;
		case 'C':
			if (flags & CLIENT_CONTROL)
				flags |= CLIENT_CONTROLCONTROL;
			else
				flags |= CLIENT_CONTROL;
			break;
		case 'f':
			if (!fflag) {
				fflag = 1;
				for (i = 0; i < cfg_nfiles; i++)
					free(cfg_files[i]);
				cfg_nfiles = 0;
			}
			cfg_files = xreallocarray(cfg_files, cfg_nfiles + 1,
			    sizeof *cfg_files);
			cfg_files[cfg_nfiles++] = xstrdup(optarg);
			cfg_quiet = 0;
			break;
		case 'h':
			usage(0);
		case 'V':
			printf("tmux %s\n", getversion());
			exit(0);
		case 'l':
			flags |= CLIENT_LOGIN;
			break;
		case 'L':
			free(label);
			label = xstrdup(optarg);
			break;
		case 'N':
			flags |= CLIENT_NOSTARTSERVER;
			break;
		case 'q':
			break;
		case 'S':
			free(path);
			path = xstrdup(optarg);
			break;
		case 'T':
			tty_add_features(&feat, optarg, ":,");
			break;
		case 'u':
			flags |= CLIENT_UTF8;
			break;
		case 'v':
			log_add_level();
			break;
		default:
			usage(1);
		}
	}
	argc -= optind;
	argv += optind;

#ifdef PLATFORM_WINDOWS
        win32_log("After getopt\n");
#endif

	if (shell_command != NULL && argc != 0)
		usage(1);
	if ((flags & CLIENT_NOFORK) && argc != 0)
		usage(1);

#ifdef PLATFORM_WINDOWS
        win32_log("Calling getptmfd\n");
#endif
	if ((ptm_fd = getptmfd()) == -1) // <--- Check this
		err(1, "getptmfd");
#ifdef PLATFORM_WINDOWS
        win32_log("getptmfd success: %d\n", ptm_fd);
#endif

#ifdef PLATFORM_WINDOWS
        win32_log("Calling pledge\n");
#endif
    /*
	if (pledge("stdio rpath wpath cpath flock fattr unix getpw sendfd "
	    "recvfd proc exec tty ps", NULL) != 0)
		err(1, "pledge");
    */
    /* Pledge is mostly OpenBSD specific, verify if stumped or unsafe */
    /* Commenting out for now to ensure it's not the blocker, or check definition */
#ifdef PLATFORM_WINDOWS
        win32_log("Skipping pledge check or handled\n");
#endif

	/*
	 * tmux is a UTF-8 terminal, so if TMUX is set, assume UTF-8.
	 * Otherwise, if the user has set LC_ALL, LC_CTYPE or LANG to contain
	 * UTF-8, it is a safe assumption that either they are using a UTF-8
	 * terminal, or if not they know that output from UTF-8-capable
	 * programs may be wrong.
	 */
	if (getenv("TMUX") != NULL)
		flags |= CLIENT_UTF8;
	else {
		s = getenv("LC_ALL");
		if (s == NULL || *s == '\0')
			s = getenv("LC_CTYPE");
		if (s == NULL || *s == '\0')
			s = getenv("LANG");
		if (s == NULL || *s == '\0')
			s = "";
		if (strcasestr(s, "UTF-8") != NULL ||
		    strcasestr(s, "UTF8") != NULL)
			flags |= CLIENT_UTF8;
	}

	global_options = options_create(NULL);
	global_s_options = options_create(NULL);
	global_w_options = options_create(NULL);
	for (oe = options_table; oe->name != NULL; oe++) {
		if (oe->scope & OPTIONS_TABLE_SERVER)
			options_default(global_options, oe);
		if (oe->scope & OPTIONS_TABLE_SESSION)
			options_default(global_s_options, oe);
		if (oe->scope & OPTIONS_TABLE_WINDOW)
			options_default(global_w_options, oe);
	}

	/*
	 * The default shell comes from SHELL or from the user's passwd entry
	 * if available.
	 */
	options_set_string(global_s_options, "default-shell", 0, "%s",
	    getshell());

	/* Override keys to vi if VISUAL or EDITOR are set. */
	if ((s = getenv("VISUAL")) != NULL || (s = getenv("EDITOR")) != NULL) {
		options_set_string(global_options, "editor", 0, "%s", s);
		if (strrchr(s, '/') != NULL)
			s = strrchr(s, '/') + 1;
		if (strstr(s, "vi") != NULL)
			keys = MODEKEY_VI;
		else
			keys = MODEKEY_EMACS;
		options_set_number(global_s_options, "status-keys", keys);
		options_set_number(global_w_options, "mode-keys", keys);
	}

	/*
	 * If socket is specified on the command-line with -S or -L, it is
	 * used. Otherwise, $TMUX is checked and if that fails "default" is
	 * used.
	 */
	if (path == NULL && label == NULL) {
		s = getenv("TMUX");
		if (s != NULL && *s != '\0' && *s != ',') {
			path = xstrdup(s);
			path[strcspn(path, ",")] = '\0';
		}
	}
	if (path == NULL) {
#ifdef PLATFORM_WINDOWS
        win32_log("Calling make_label\n");
#endif
		if ((path = make_label(label, &cause)) == NULL) {
#ifdef PLATFORM_WINDOWS
             win32_log("make_label failed: %s\n", cause);
#endif
			if (cause != NULL) {
				fprintf(stderr, "%s\n", cause);
				free(cause);
			}
			exit(1);
		}
#ifdef PLATFORM_WINDOWS
        win32_log("make_label success: %s\n", path);
#endif
		flags |= CLIENT_DEFAULTSOCKET;
	}
	socket_path = path;
#ifdef PLATFORM_WINDOWS
	if (socket_path != NULL) {
        win32_log("Calling win32_translate_socket_path: %s\n", socket_path);
		char *new_path = win32_translate_socket_path(socket_path);
        win32_log("Translated path: %s\n", new_path);
		if (new_path != socket_path) {
			free(path);
			socket_path = xstrdup(new_path);
		}
	}
#endif
	free(label);

	/* Pass control to the client. */
#ifdef PLATFORM_WINDOWS
    win32_log("Calling osdep_event_init\n");
#endif
	struct event_base *base = osdep_event_init(); // Keep base local

#ifdef PLATFORM_WINDOWS
    if (argc > 0 && strcmp(argv[0], "__win32_server") == 0) {
        win32_log("Starting server_child_main\n");
        int lockfd = -1;
        char *lockfile = NULL;
        const char *ready_event_name = NULL;

        /* argv[1] is the optional ready-event name from server_start(). */
        if (argc > 1 && argv[1] != NULL)
            ready_event_name = argv[1];

        if (socket_path) {
            xasprintf(&lockfile, "%s.lock", socket_path);
            lockfd = open(lockfile, O_WRONLY|O_CREAT, 0600);
            if (lockfd != -1) {
                 if (flock(lockfd, LOCK_EX) == -1) {
                     close(lockfd);
                     lockfd = -1;
                 }
            }
        }
        server_child_main(NULL, 0, base, lockfd, lockfile,
            ready_event_name);
        exit(0);
    }
#endif

#ifdef PLATFORM_WINDOWS
    win32_log("Calling client_main\n");
	exit(client_main(base, argc, argv, flags, feat));
#else
	exit(client_main(base, argc, argv, flags, feat)); // POSIX uses osdep_event_init() result directly
#endif
}
