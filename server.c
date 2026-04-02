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
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>

#ifdef _MSC_VER
#pragma warning(disable:4113)
#endif

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "tmux.h"

/*
 * Main server functions.
 */

struct clients		 clients;

struct tmuxproc		*server_proc;
static int		 server_fd = -1;
static uint64_t		 server_client_flags;
static int		 server_exit;
static struct event	 server_ev_accept;
static struct event	 server_ev_tidy;

struct cmd_find_state	 marked_pane;

static u_int		 message_next;
struct message_list	 message_log;

time_t			 current_time;

static int	server_loop(void);
static void	server_send_exit(void);
static void	server_accept(int, short, void *);
static void	server_signal(int);
static void	server_child_signal(void);
static void	server_child_exited(pid_t, int);
static void	server_child_stopped(pid_t, int);

/* Set marked pane. */
void
server_set_marked(struct session *s, struct winlink *wl, struct window_pane *wp)
{
	cmd_find_clear_state(&marked_pane, 0);
	marked_pane.s = s;
	marked_pane.wl = wl;
	if (wl != NULL)
		marked_pane.w = wl->window;
	marked_pane.wp = wp;
}

/* Clear marked pane. */
void
server_clear_marked(void)
{
	cmd_find_clear_state(&marked_pane, 0);
}

/* Is this the marked pane? */
int
server_is_marked(struct session *s, struct winlink *wl, struct window_pane *wp)
{
	if (s == NULL || wl == NULL || wp == NULL)
		return (0);
	if (marked_pane.s != s || marked_pane.wl != wl)
		return (0);
	if (marked_pane.wp != wp)
		return (0);
	return (server_check_marked());
}

/* Check if the marked pane is still valid. */
int
server_check_marked(void)
{
	return (cmd_find_valid_state(&marked_pane));
}

/* Create server socket. */
int
server_create_socket(uint64_t flags, char **cause)
{
	struct sockaddr_un	sa;
	size_t			size;
	mode_t			mask;
	int			fd, saved_errno;

#ifdef PLATFORM_WINDOWS
    win32_log("server_create_socket: entered, path=%s\n", socket_path);
#endif

	memset(&sa, 0, sizeof sa);
	sa.sun_family = AF_UNIX;
	size = strlcpy(sa.sun_path, socket_path, sizeof sa.sun_path);
	if (size >= sizeof sa.sun_path) {
		errno = ENAMETOOLONG;
		goto fail;
	}
	unlink(sa.sun_path);

	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
		goto fail;

	if (flags & CLIENT_DEFAULTSOCKET)
		mask = umask(S_IXUSR|S_IXGRP|S_IRWXO);
	else
		mask = umask(S_IXUSR|S_IRWXG|S_IRWXO);
	if (bind(fd, (struct sockaddr *)&sa, sizeof sa) == -1) {
		saved_errno = errno;
#ifdef PLATFORM_WINDOWS
    win32_log("server_create_socket: bind failed %d\n", saved_errno);
#endif
		close(fd);
		errno = saved_errno;
		goto fail;
	}
	umask(mask);

	if (listen(fd, 128) == -1) {
		saved_errno = errno;
#ifdef PLATFORM_WINDOWS
    win32_log("server_create_socket: listen failed %d\n", saved_errno);
#endif
		close(fd);
		errno = saved_errno;
		goto fail;
	}
#ifdef PLATFORM_WINDOWS
	/* Make listening socket non-blocking so accept() returns EWOULDBLOCK
	 * when no connection is pending (timer-poll accept pattern). */
	{
		u_long nb = 1;
		SOCKET s = win32_get_real_socket(fd);
		ioctlsocket(s, FIONBIO, &nb);
	}
    win32_log("server_create_socket: success, fd=%d\n", fd);
#endif
	setblocking(fd, 0);

	return (fd);

fail:
	if (cause != NULL) {
		xasprintf(cause, "error creating %s (%s)", socket_path,
		    strerror(errno));
	}
	return (-1);
}

/* Tidy up every hour. */
static void
server_tidy_event(__unused int fd, __unused short events, __unused void *data)
{
    struct timeval	tv = { .tv_sec = 3600 };
    uint64_t		t = get_timer();

    format_tidy_jobs();

#ifdef HAVE_MALLOC_TRIM
    malloc_trim(0);
#endif

    log_debug("%s: took %llu milliseconds", __func__,
        (unsigned long long)(get_timer() - t));
    evtimer_add(&server_ev_tidy, &tv);
}

/* Fork new server. */
int
server_start(struct tmuxproc *client, uint64_t flags, struct event_base *base,
    int lockfd, char *lockfile)
{
#ifdef PLATFORM_WINDOWS
    win32_log("server_start: entered with lockfd=%d, lockfile=%s\n", lockfd, lockfile ? lockfile : "NULL");
#endif
	int		 fd;
	sigset_t	 set, oldset;
	sigfillset(&set);
	sigprocmask(SIG_BLOCK, &set, &oldset);
#ifdef PLATFORM_WINDOWS
    win32_log("server_start: signal mask done\n");
#endif

#ifdef PLATFORM_WINDOWS
    /* Windows implementation: Spawn new process */
    char *cmdline;
    char exe_path[MAX_PATH];
    GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    win32_log("server_start: exe_path=%s\n", exe_path);

    /*
     * Create a named event so the client can wait for the server to
     * finish bind()+listen() instead of using a fixed Sleep().  The
     * event name is passed on the command line; the server opens it
     * by name and signals it once the listening socket is ready.
     */
    char evname[64];
    HANDLE ready_event;
    snprintf(evname, sizeof evname, "tmux-ready-%lu",
        (unsigned long)GetCurrentProcessId());
    ready_event = CreateEventA(NULL, TRUE, FALSE, evname);
    if (ready_event == NULL) {
        win32_log("server_start: CreateEvent failed %lu\n",
            (unsigned long)GetLastError());
        sigprocmask(SIG_SETMASK, &oldset, NULL);
        return -1;
    }

    /* Build command line, forwarding -f config files and -S socket path. */
    {
        char *p = NULL;
        u_int i;

        xasprintf(&p, "\"%s\"", exe_path);
        if (socket_path) {
            char *tmp;
            xasprintf(&tmp, "%s -S \"%s\"", p, socket_path);
            free(p);
            p = tmp;
        }
        for (i = 0; i < cfg_nfiles; i++) {
            char *tmp;
            xasprintf(&tmp, "%s -f \"%s\"", p, cfg_files[i]);
            free(p);
            p = tmp;
        }
        {
            char *tmp;
            xasprintf(&tmp, "%s __win32_server %s", p, evname);
            free(p);
            cmdline = tmp;
        }
    }

    win32_log("server_start: cmdline=%s\n", cmdline);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));

    /* Do NOT inherit handles: the client's stdout/stderr pipes (from
     * Start-Process -RedirectStandardOutput) must not leak into the server
     * process, or the pipe will never reach EOF and callers will hang. */
    if (!CreateProcessA(NULL, cmdline, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        win32_log("server_start: CreateProcess failed %lu\n",
            (unsigned long)GetLastError());
        free(cmdline);
        CloseHandle(ready_event);
        sigprocmask(SIG_SETMASK, &oldset, NULL);
        return -1;
    }
    win32_log("server_start: child pid %lu\n",
        (unsigned long)pi.dwProcessId);
    free(cmdline);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    /*
     * Wait for the server to signal readiness (bind+listen done)
     * instead of a fixed Sleep(300).  Fall back to 5s timeout so we
     * don't hang forever if the server crashes during init.
     */
    DWORD wait_result = WaitForSingleObject(ready_event, 5000);
    CloseHandle(ready_event);
    if (wait_result == WAIT_OBJECT_0) {
        win32_log("server_start: server signalled ready\n");
    } else {
        win32_log("server_start: ready-event wait returned %lu "
            "(timeout or error)\n", (unsigned long)wait_result);
    }

    if (lockfd >= 0) {
        win32_log("server_start: NOT closing lockfd here, letting caller handle it\n");
    }

    sigprocmask(SIG_SETMASK, &oldset, NULL);
    return 0;

#else /* POSIX implementation */

	if (~flags & CLIENT_NOFORK) {
		if (proc_fork_and_daemon(&fd) != 0) {
			sigprocmask(SIG_SETMASK, &oldset, NULL);
			return (fd);
		}
	}
	// ... continue in child ...
    server_child_main(client, flags, base, lockfd, lockfile, NULL);
    // server_child_main exits logic?
    exit(0);
#endif
}

/* Extracted child logic */
void
server_child_main(struct tmuxproc *client, uint64_t flags, struct event_base *base,
     int lockfd, char *lockfile, const char *ready_event_name)
{
	int		 fd = -1; // No FD passing on Windows/Child?
    // Wait. On POSIX `fd` comes from `proc_fork_and_daemon`.
    // On Windows, `fd` (socket pair) doesn't exist.
    // The server needs to create the listening socket.
    // `server_create_socket` does that.
    
    sigset_t	 set, oldset;
    struct client *c = NULL;
    char *cause = NULL;
    struct timeval tv = { .tv_sec = 3600 };
    
	sigfillset(&set);
	sigprocmask(SIG_BLOCK, &set, &oldset);
#ifdef PLATFORM_WINDOWS
    win32_log("server_child_main: signal mask done\n");
#endif

    if (client != NULL)
	    proc_clear_signals(client, 0);
	server_client_flags = flags;
#ifdef PLATFORM_WINDOWS
    win32_log("server_child_main: calling event_reinit\n");
#endif

#ifndef PLATFORM_WINDOWS
	if (event_reinit(base) != 0)
		fatalx("event_reinit failed");
#endif

#ifdef PLATFORM_WINDOWS
    win32_log("server_child_main: event_reinit done\n");
#endif
	server_proc = proc_start("server");

	proc_set_signals(server_proc, server_signal);
	sigprocmask(SIG_SETMASK, &oldset, NULL);

	if (log_get_level() > 1)
		tty_create_log();
#ifdef PLATFORM_WINDOWS
    win32_log("server_child_main: attempting pledge\n");
#endif
	if (pledge("stdio rpath wpath cpath fattr unix getpw recvfd proc exec "
	    "tty ps", NULL) != 0)
		fatal("pledge failed");
#ifdef PLATFORM_WINDOWS
    win32_log("server_child_main: pledge done\n");
#endif

	input_key_build();
	utf8_update_width_cache();
	RB_INIT(&windows);
	RB_INIT(&all_window_panes);
	TAILQ_INIT(&clients);
	RB_INIT(&sessions);
	key_bindings_init();
	TAILQ_INIT(&message_log);
	gettimeofday(&start_time, NULL);

#ifdef HAVE_SYSTEMD
	server_fd = systemd_create_socket(flags, &cause);
#else
	server_fd = server_create_socket(flags, &cause);
#endif
#ifndef PLATFORM_WINDOWS
	if (server_fd != -1)
		server_update_socket();
#endif
#ifdef PLATFORM_WINDOWS
    win32_log("server_child_main: socket updated (skipped chmod)\n");
#endif
    
    /* On Windows/Proxy mode, we don't have the inherited client connection `fd` here.
       Clients connect via socket. `server_accept` handles them.
       So we skip `server_client_create(fd)`.
    */
#ifndef PLATFORM_WINDOWS
	if (~flags & CLIENT_NOFORK)
		c = server_client_create(fd); // fd is undefined here if extracted?
        // Ah, `fd` was local in `server_start`.
        // We need `fd` passed in?
        // `fd` is the OTHER end of socket pair?
#else
    // Windows: No pre-connected client. Client will connect.
    // Ensure we don't exit immediately due to no clients.
    // We set exit-empty?
    // `options_set_number(global_options, "exit-empty", 0);` logic below handles NOFORK.
    // We treat Windows logic similar to NOFORK initially?
    // No, client WILL connect.
#endif

#ifdef PLATFORM_WINDOWS
    win32_log("server_child_main: entering main loop\n");
#endif

    // The logic below for `c` expects `c` to be set?
    /*
	if (~flags & CLIENT_NOFORK)
		c = server_client_create(fd);
	else
		options_set_number(global_options, "exit-empty", 0);
    */
    // On Windows, treat as NOFORK equivalent for initialization (start listener, wait for client)
    // But `flags & CLIENT_NOFORK` is false (default).
    // We should FORCE `exit-empty` 0?
    // Or assume client connects fast enough?
    // Better to set exit-empty 0?
    options_set_number(global_options, "exit-empty", 0);

	if (lockfd >= 0) {
		unlink(lockfile); // Server removes lock file?
		free(lockfile);
		close(lockfd);
	}

	if (cause != NULL) {
		if (c != NULL) {
			c->exit_message = cause;
			c->flags |= CLIENT_EXIT;
		} else {
			fprintf(stderr, "%s\n", cause);
			exit(1);
		}
	}

	evtimer_set(&server_ev_tidy, server_tidy_event, NULL);
	evtimer_add(&server_ev_tidy, &tv);

	server_acl_init();

	server_add_accept(0);
#ifdef PLATFORM_WINDOWS
    /*
     * Signal the client that the listening socket is ready.
     * The client created a named event and passed its name on the
     * command line; we open it by name and set it.  This replaces
     * the old Sleep(300) + retry loop.
     */
    if (ready_event_name != NULL) {
        HANDLE ev = OpenEventA(EVENT_MODIFY_STATE, FALSE,
            ready_event_name);
        if (ev != NULL) {
            SetEvent(ev);
            CloseHandle(ev);
            win32_log("server_child_main: signalled ready event '%s'\n",
                ready_event_name);
        } else {
            win32_log("server_child_main: OpenEvent('%s') failed %lu\n",
                ready_event_name, (unsigned long)GetLastError());
        }
    }

    if (win32_job_init() != 0)
        win32_log("server_child_main: win32_job_init failed (if-shell will not work)\n");
    win32_session_log_init();
    win32_log("server_child_main: entering proc_loop\n");
#endif
	proc_loop(server_proc, server_loop);
#ifdef PLATFORM_WINDOWS
    win32_log("server_child_main: exited proc_loop\n");
#endif

	job_kill_all();
	status_prompt_save_history();

	exit(0);
}

/* Server loop callback. */
static int
server_loop(void)
{
	struct client	*c;
	u_int		 items;

	current_time = time(NULL);

#ifdef PLATFORM_WINDOWS
    win32_log("server_loop: entry\n");
#endif

	do {
#ifdef PLATFORM_WINDOWS
        win32_log("server_loop: calling cmdq_next(NULL)\n");
#endif
		items = cmdq_next(NULL);
#ifdef PLATFORM_WINDOWS
        if (items) win32_log("server_loop: cmdq_next(NULL) processed %u items\n", items);
#endif
		TAILQ_FOREACH(c, &clients, entry) {
			if (c->flags & CLIENT_IDENTIFIED) {
#ifdef PLATFORM_WINDOWS
                win32_log("server_loop: calling cmdq_next(client %p)\n", c);
#endif
				u_int n = cmdq_next(c);
#ifdef PLATFORM_WINDOWS
                if (n) win32_log("server_loop: cmdq_next(client %p) processed %u items\n", c, n);
#endif
                items += n;
            }
		}
	} while (items != 0);

#ifdef PLATFORM_WINDOWS
	win32_log("server_loop: do-while exited (items=0)\n");
	win32_log("server_loop: calling server_client_loop\n");
#endif
	server_client_loop();
#ifdef PLATFORM_WINDOWS
	win32_log("server_loop: server_client_loop returned\n");
#endif

	if (!options_get_number(global_options, "exit-empty") && !server_exit)
		return (0);

	if (!options_get_number(global_options, "exit-unattached")) {
		if (!RB_EMPTY(&sessions))
			return (0);
	}

	if (!options_get_number(global_options, "exit-unattached")) {
		if (!RB_EMPTY(&sessions))
			return (0);
	}

	TAILQ_FOREACH(c, &clients, entry) {
		if (c->session != NULL)
			return (0);
	}

	/*
	 * No attached clients therefore want to exit - flush any waiting
	 * clients but don't actually exit until they've gone.
	 */
	cmd_wait_for_flush();
	if (!TAILQ_EMPTY(&clients))
		return (0);

	if (job_still_running())
		return (0);

	return (1);
}

/* Exit the server by killing all clients and windows. */
static void
server_send_exit(void)
{
	struct client	*c, *c1;
	struct session	*s, *s1;

	cmd_wait_for_flush();

	TAILQ_FOREACH_SAFE(c, &clients, entry, c1) {
		if (c->flags & CLIENT_SUSPENDED)
			server_client_lost(c);
		else {
			c->flags |= CLIENT_EXIT;
			c->exit_type = CLIENT_EXIT_SHUTDOWN;
		}
		c->session = NULL;
	}

	RB_FOREACH_SAFE(s, sessions, &sessions, s1)
		session_destroy(s, 1, __func__);
}

/* Update socket execute permissions based on whether sessions are attached. */
void
server_update_socket(void)
{
	struct session	*s;
	static int	 last = -1;
	int		 n, mode;
	struct stat      sb;

	n = 0;
	RB_FOREACH(s, sessions, &sessions) {
		if (s->attached != 0) {
			n++;
			break;
		}
	}

	if (n != last) {
		last = n;

		if (stat(socket_path, &sb) != 0)
			return;
		mode = sb.st_mode & ACCESSPERMS;
		if (n != 0) {
			if (mode & S_IRUSR)
				mode |= S_IXUSR;
			if (mode & S_IRGRP)
				mode |= S_IXGRP;
			if (mode & S_IROTH)
				mode |= S_IXOTH;
		} else
			mode &= ~(S_IXUSR|S_IXGRP|S_IXOTH);
		chmod(socket_path, mode);
	}
}

/* Callback for server socket. */
static void
server_accept(int fd, short events, __unused void *data)
{
	struct sockaddr_storage	 sa;
	socklen_t		 slen = sizeof sa;
	int			 newfd;
	struct client		*c;

#ifdef PLATFORM_WINDOWS
    win32_log("server_accept: entered, fd (int)=%d, raw_fd (long long)=%lld, events=%d\n", 
              fd, (long long)*(intptr_t*)(&fd), events);
#endif

	server_add_accept(0);
#ifndef PLATFORM_WINDOWS
	if (!(events & EV_READ))
		return;
#endif
	/* On Windows, always try accept() — we use timer polling, not EV_READ */

	newfd = accept(fd, (struct sockaddr *) &sa, &slen);
	if (newfd == -1) {
		if (errno == EAGAIN || errno == EINTR || errno == ECONNABORTED)
			return;
#ifdef PLATFORM_WINDOWS
		/* WSAEWOULDBLOCK means no connection pending — normal for timer poll */
		if (errno == EWOULDBLOCK)
			return;
#endif
		if (errno == ENFILE || errno == EMFILE) {
			/* Delete and don't try again for 1 second. */
			server_add_accept(1);
			return;
		}
		fatal("accept failed");
	}

	if (server_exit) {
		win32_log("server_accept: server_exit is set, closing newfd=%d\n", newfd);
		close(newfd);
		return;
	}
#ifdef PLATFORM_WINDOWS
	if (!win32_check_peer_is_owner(newfd)) {
		win32_log("server_accept: peer SID mismatch, rejecting newfd=%d\n", newfd);
		close(newfd);
		return;
	}
#endif
	win32_log("server_accept: calling server_client_create(%d)\n", newfd);
	c = server_client_create(newfd);
	win32_log("server_accept: server_client_create returned %p\n", c);
	if (!server_acl_join(c)) {
		win32_log("server_accept: ACL join failed for client %p\n", c);
		c->exit_message = xstrdup("access not allowed");
		c->flags |= CLIENT_EXIT;
	}
}

/*
 * Add accept event. If timeout is nonzero, add as a timeout instead of a read
 * event - used to backoff when running out of file descriptors.
 */
void
server_add_accept(int timeout)
{
	struct timeval tv = { timeout, 0 };

	if (server_fd == -1)
		return;

	if (event_initialized(&server_ev_accept))
		event_del(&server_ev_accept);

#ifdef PLATFORM_WINDOWS
	/*
	 * Winsock select() does not support AF_UNIX sockets, so EV_READ on the
	 * listening socket never fires.  Use a 50ms polling timer instead so
	 * server_accept is called frequently and can pick up new connections.
	 */
	{
		struct timeval poll_tv = { 0, timeout == 0 ? 50000 : timeout * 1000000 };
		event_set(&server_ev_accept, server_fd, EV_TIMEOUT, server_accept, NULL);
		event_add(&server_ev_accept, &poll_tv);
	}
#else
	if (timeout == 0) {
		event_set(&server_ev_accept, server_fd, EV_READ, server_accept,
		    NULL);
		event_add(&server_ev_accept, NULL);
	} else {
		event_set(&server_ev_accept, server_fd, EV_TIMEOUT,
		    server_accept, NULL);
		event_add(&server_ev_accept, &tv);
	}
#endif
}

/* Signal handler. */
static void
server_signal(int sig)
{
	int	fd;

	log_debug("%s: %s", __func__, strsignal(sig));
	switch (sig) {
	case SIGINT:
	case SIGTERM:
		server_exit = 1;
		server_send_exit();
		break;
	case SIGCHLD:
		server_child_signal();
		break;
	case SIGUSR1:
		event_del(&server_ev_accept);
		fd = server_create_socket(server_client_flags, NULL);
		if (fd != -1) {
			close(server_fd);
			server_fd = fd;
			server_update_socket();
		}
		server_add_accept(0);
		break;
	case SIGUSR2:
		proc_toggle_log(server_proc);
		break;
	}
}

/* Handle SIGCHLD. */
static void
server_child_signal(void)
{
	int	 status;
	pid_t	 pid;

	for (;;) {
		switch (pid = waitpid(WAIT_ANY, &status, WNOHANG|WUNTRACED)) {
		case -1:
			if (errno == ECHILD)
				return;
			fatal("waitpid failed");
		case 0:
			return;
		}
		if (WIFSTOPPED(status))
			server_child_stopped(pid, status);
		else if (WIFEXITED(status) || WIFSIGNALED(status))
			server_child_exited(pid, status);
	}
}

/* Handle exited children. */
static void
server_child_exited(pid_t pid, int status)
{
	struct window		*w, *w1;
	struct window_pane	*wp;

	RB_FOREACH_SAFE(w, windows, &windows, w1) {
		TAILQ_FOREACH(wp, &w->panes, entry) {
			if (wp->pid == pid) {
				wp->status = status;
				wp->flags |= PANE_STATUSREADY;

				log_debug("%%%u exited", wp->id);
				wp->flags |= PANE_EXITED;

				if (window_pane_destroy_ready(wp))
					server_destroy_pane(wp, 1);
				break;
			}
		}
	}
	job_check_died(pid, status);
}

/* Handle stopped children. */
static void
server_child_stopped(pid_t pid, int status)
{
	struct window		*w;
	struct window_pane	*wp;

	if (WSTOPSIG(status) == SIGTTIN || WSTOPSIG(status) == SIGTTOU)
		return;

	RB_FOREACH(w, windows, &windows) {
		TAILQ_FOREACH(wp, &w->panes, entry) {
			if (wp->pid == pid) {
				if (killpg(pid, SIGCONT) != 0)
					kill(pid, SIGCONT);
			}
		}
	}
	job_check_died(pid, status);
}

/* Add to message log. */
void
server_add_message(const char *fmt, ...)
{
	struct message_entry	*msg, *msg1;
	char			*s;
	va_list			 ap;
	u_int			 limit;

	va_start(ap, fmt);
	xvasprintf(&s, fmt, ap);
	va_end(ap);

	log_debug("message: %s", s);

	msg = xcalloc(1, sizeof *msg);
	gettimeofday(&msg->msg_time, NULL);
	msg->msg_num = message_next++;
	msg->msg = s;
	TAILQ_INSERT_TAIL(&message_log, msg, entry);

	limit = options_get_number(global_options, "message-limit");
	TAILQ_FOREACH_SAFE(msg, &message_log, entry, msg1) {
		if (msg->msg_num + limit >= message_next)
			break;
		free(msg->msg);
		TAILQ_REMOVE(&message_log, msg, entry);
		free(msg);
	}
}
