/*
 * win32.h - Master Windows compatibility header for tmux
 *
 * This header provides POSIX compatibility definitions and function
 * declarations for the Windows port of tmux.
 */

#ifndef COMPAT_WIN32_H
#define COMPAT_WIN32_H

/* Must be included before windows.h */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00  /* Windows 10 */
#endif

#include "config.h"

#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#define closefrom win32_closefrom
#define daemon win32_daemon

/* File offset macros */
#define fseeko _fseeki64
#define ftello _ftelli64

/* Time re-entrant macros */
#define localtime_r(t, tm) (_localtime64_s((tm), (const __time64_t *)(t)) == 0 ? (tm) : NULL)
#define gmtime_r(t, tm) (_gmtime64_s((tm), (const __time64_t *)(t)) == 0 ? (tm) : NULL)
#define ctime_r(t, buf) (ctime_s((buf), 26, (t)) == 0 ? (buf) : NULL)

/* Prototypes for missing functions */
int mkstemp(char *template);
char *strsignal(int sig);

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

/* Winsock2 must come before windows.h */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <afunix.h>
#include <windows.h>
#include <io.h>
#include <process.h>
#include <direct.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>

/* Undefine environ macro to avoid conflict with struct environ in tmux.h */
#ifdef environ
#undef environ
#endif
extern char **environ;

/* Include libevent early to provide full structure definitions */
#include <event.h>

/* Structure for timespec if missing */
#ifndef _TIMESPEC_DEFINED
#define _TIMESPEC_DEFINED
struct timespec {
    time_t tv_sec;
    long   tv_nsec;
};
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Basic type definitions
 */
typedef int pid_t;
typedef int uid_t;
typedef int gid_t;
typedef int mode_t;
typedef long ssize_t;
typedef char * caddr_t;
typedef unsigned int u_int;
typedef unsigned char u_char;
typedef unsigned short u_short;
typedef unsigned long u_long;

/*
 * File descriptor constants
 */
#ifndef STDIN_FILENO
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#endif

/*
 * File mode bits
 */
#ifndef S_IRWXU
#define S_IRWXU 0700
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRWXG 0070
#define S_IRGRP 0040
#define S_IWGRP 0200
#define S_IXGRP 0100
#define S_IRWXO 0007
#define S_IROTH 0004
#define S_IWOTH 0002
#define S_IXOTH 0001
#endif

#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISLNK(m) (0)  /* No symlinks in basic Windows */
#define S_ISSOCK(m) (0)
#define S_ISFIFO(m) (0)
#endif

#ifndef ACCESSPERMS
#define ACCESSPERMS (S_IRWXU|S_IRWXG|S_IRWXO)
#endif

/*
 * fcntl constants
 */
#ifndef F_GETFL
#define F_GETFL 3
#define F_SETFL 4
#define F_GETFD 1
#define F_SETFD 2
#define FD_CLOEXEC 1
#define O_NONBLOCK 0x4000
#define O_CLOEXEC  0x80000
#define O_DIRECTORY 0x100000
#define O_NOCTTY   0x8000
#endif

/*
 * Wait status macros
 */
#ifndef WIFEXITED
#define WIFEXITED(status)   (((status) & 0x7f) == 0)
#define WEXITSTATUS(status) (((status) >> 8) & 0xff)
#define WIFSIGNALED(status) (((status) & 0x7f) != 0 && ((status) & 0x7f) != 0x7f)
#define WTERMSIG(status)    ((status) & 0x7f)
#define WIFSTOPPED(status)  (((status) & 0xff) == 0x7f)
#define WSTOPSIG(status)    (((status) >> 8) & 0xff)
#endif

#ifndef WAIT_ANY
#define WAIT_ANY (-1)
#endif

/* waitpid options */
#ifndef WNOHANG
#define WNOHANG   1
#define WUNTRACED 2
#define WCONTINUED 8
#endif

/*
 * Signal definitions (emulated)
 */
#ifndef SIGHUP
#define SIGHUP    1
#endif
#ifndef SIGINT
#define SIGINT    2
#endif
#ifndef SIGQUIT
#define SIGQUIT   3
#endif
#ifndef SIGILL
#define SIGILL    4
#endif
#ifndef SIGTRAP
#define SIGTRAP   5
#endif
#ifndef SIGABRT
#define SIGABRT   6
#endif
#ifndef SIGBUS
#define SIGBUS    7
#endif
#ifndef SIGFPE
#define SIGFPE    8
#endif
#ifndef SIGKILL
#define SIGKILL   9
#endif
#ifndef SIGUSR1
#define SIGUSR1   10
#endif
#ifndef SIGUSR2
#define SIGUSR2   12
#endif
#ifndef SIGPIPE
#define SIGPIPE   13
#endif
#ifndef SIGALRM
#define SIGALRM   14
#endif
#ifndef SIGTERM
#define SIGTERM   15
#endif
#ifndef SIGSTKFLT
#define SIGSTKFLT 16
#endif
#ifndef SIGCHLD
#define SIGCHLD   17
#endif
#ifndef SIGCONT
#define SIGCONT   18
#endif
#ifndef SIGSTOP
#define SIGSTOP   19
#endif
#ifndef SIGTSTP
#define SIGTSTP   20
#endif
#ifndef SIGTTIN
#define SIGTTIN   21
#endif
#ifndef SIGTTOU
#define SIGTTOU   22
#endif
#ifndef SIGURG
#define SIGURG    23
#endif
#ifndef SIGXCPU
#define SIGXCPU   24
#endif
#ifndef SIGXFSZ
#define SIGXFSZ   25
#endif
#ifndef SIGVTALRM
#define SIGVTALRM 26
#endif
#ifndef SIGPROF
#define SIGPROF   27
#endif
#ifndef SIGWINCH
#define SIGWINCH  28
#endif
#ifndef SIGIO
#define SIGIO     29
#endif
#ifndef SIGPWR
#define SIGPWR    30
#endif
#ifndef SIGSYS
#define SIGSYS    31
#endif

#ifndef NSIG
#define NSIG 32
#endif

#ifndef SIGBREAK
#define SIGBREAK 21
#endif

typedef void (*sighandler_t)(int);
typedef uint32_t sigset_t;

struct sigaction {
    sighandler_t sa_handler;
    sigset_t sa_mask;
    int sa_flags;
};

#define SA_RESTART  0x10000000
#define SA_NOCLDSTOP 0x00000001
#define SA_NOCLDWAIT 0x00000002
#define SA_SIGINFO   0x00000004

#ifndef SIG_DFL
#define SIG_DFL ((sighandler_t)0)
#endif
#ifndef SIG_IGN
#define SIG_IGN ((sighandler_t)1)
#endif
#ifndef SIG_ERR
#define SIG_ERR ((sighandler_t)-1)
#endif

/* sigprocmask how values */
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

/*
 * Socket compatibility
 */
#ifndef AF_LOCAL
#define AF_LOCAL AF_UNIX
#endif

#ifndef PF_UNIX
#define PF_UNIX AF_UNIX
#endif

#ifndef PF_LOCAL
#define PF_LOCAL AF_UNIX
#endif

#ifndef SHUT_RD
#define SHUT_RD   SD_RECEIVE
#define SHUT_WR   SD_SEND
#define SHUT_RDWR SD_BOTH
#endif

/*
 * ioctl definitions for terminal
 */
#ifndef TIOCGWINSZ
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define TIOCSCTTY  0x540E
#define TIOCNOTTY  0x5422
#endif

/*
 * termios placeholder structures
 */
struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

typedef unsigned int tcflag_t;
typedef unsigned char cc_t;
typedef unsigned int speed_t;

#define NCCS 32

/* POSIX VDISABLE */
#ifndef _POSIX_VDISABLE
#define _POSIX_VDISABLE 0
#endif

struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t c_cc[NCCS];
    speed_t c_ispeed;
    speed_t c_ospeed;
};

/* termios c_iflag bits */
#define IGNBRK  0x0001
#define BRKINT  0x0002
#define IGNPAR  0x0004
#define PARMRK  0x0008
#define INPCK   0x0010
#define ISTRIP  0x0020
#define INLCR   0x0040
#define IGNCR   0x0080
#define ICRNL   0x0100
#define IXON    0x0200
#define IXOFF   0x0400
#define IXANY   0x0800
#define IMAXBEL 0x2000
#define IUTF8   0x4000

/* termios c_oflag bits */
#define OPOST   0x0001
#define ONLCR   0x0004

/* termios c_cflag bits */
#define CSIZE   0x0030
#define CS5     0x0000
#define CS6     0x0010
#define CS7     0x0020
#define CS8     0x0030
#define CSTOPB  0x0040
#define CREAD   0x0080
#define PARENB  0x0100
#define PARODD  0x0200
#define HUPCL   0x0400
#define CLOCAL  0x0800

/* termios c_lflag bits */
#define ISIG    0x0001
#define ICANON  0x0002
#define ECHO    0x0008
#define ECHOE   0x0010
#define ECHOK   0x0020
#define ECHONL  0x0040
#define NOFLSH  0x0080
#define TOSTOP  0x0100
#define IEXTEN  0x8000
#define ECHOCTL 0x0200
#define ECHOKE  0x0800
#define ECHOPRT 0x0400

/* termios c_cc indices */
#define VINTR    0
#define VQUIT    1
#define VERASE   2
#define VKILL    3
#define VEOF     4
#define VTIME    5
#define VMIN     6
#define VSWTC    7
#define VSTART   8
#define VSTOP    9
#define VSUSP    10
#define VEOL     11
#define VREPRINT 12
#define VDISCARD 13
#define VWERASE  14
#define VLNEXT   15
#define VEOL2    16

/* tcsetattr actions */
#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

/* Baud rates */
#define B0       0
#define B50      50
#define B75      75
#define B110     110
#define B134     134
#define B150     150
#define B200     200
#define B300     300
#define B600     600
#define B1200    1200
#define B1800    1800
#define B2400    2400
#define B4800    4800
#define B9600    9600
#define B19200   19200
#define B38400   38400
#define B57600   57600
#define B115200  115200
#define B230400  230400

/*
 * Path length limits
 */
#ifndef PATH_MAX
#define PATH_MAX MAX_PATH
#endif

#ifndef NAME_MAX
#define NAME_MAX 255
#endif

#ifndef HOST_NAME_MAX
#define HOST_NAME_MAX 255
#endif

#ifndef TTY_NAME_MAX
#define TTY_NAME_MAX 32
#endif

#ifndef LOGIN_NAME_MAX
#define LOGIN_NAME_MAX 256
#endif

/*
 * Time constants
 */
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1
#endif

#ifndef INFTIM
#define INFTIM (-1)
#endif

/*
 * Misc constants
 */
#ifndef NI_MAXHOST
#define NI_MAXHOST 1025
#endif

#ifndef MAXPATHLEN
#define MAXPATHLEN PATH_MAX
#endif

/*
 * Function declarations - POSIX functions we emulate
 */

/* Process control */
pid_t win32_fork(void);
pid_t win32_forkpty(int *amaster, char *name, struct termios *termp, struct winsize *winp);
pid_t win32_waitpid(pid_t pid, int *status, int options);
int   win32_kill(pid_t pid, int sig);
int   win32_killpg(pid_t pgrp, int sig);

/* Use macros to redirect calls */
#define fork()      win32_fork()
#define forkpty     win32_forkpty
#define waitpid     win32_waitpid
#define kill        win32_kill
#define killpg      win32_killpg

/* Signals - note: don't redefine sigaction as it conflicts with struct sigaction */
int win32_sigaction_func(int sig, const struct sigaction *act, struct sigaction *oact);
int win32_sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
int win32_sigemptyset(sigset_t *set);
int win32_sigfillset(sigset_t *set);
int win32_sigaddset(sigset_t *set, int signo);
int win32_sigdelset(sigset_t *set, int signo);
int win32_sigismember(const sigset_t *set, int signo);
int win32_raise(int sig);

/* Use inline function wrapper to avoid macro/struct conflict */
static inline int sigaction(int sig, const struct sigaction *act, struct sigaction *oact) {
    return win32_sigaction_func(sig, act, oact);
}
#define sigprocmask win32_sigprocmask
#define sigemptyset win32_sigemptyset
#define sigfillset  win32_sigfillset
#define sigaddset   win32_sigaddset
#define sigdelset   win32_sigdelset
#define sigismember win32_sigismember

/* Terminal I/O */
int win32_tcgetattr(int fd, struct termios *termios_p);
int win32_tcsetattr(int fd, int optional_actions, const struct termios *termios_p);
void win32_cfmakeraw(struct termios *termios_p);
speed_t win32_cfgetispeed(const struct termios *termios_p);
speed_t win32_cfgetospeed(const struct termios *termios_p);
int win32_cfsetispeed(struct termios *termios_p, speed_t speed);
int win32_cfsetospeed(struct termios *termios_p, speed_t speed);
int win32_ioctl(int fd, unsigned long request, ...);
int win32_isatty(int fd);
char *win32_ttyname(int fd);

#define tcgetattr   win32_tcgetattr
#define tcsetattr   win32_tcsetattr
#define cfmakeraw   win32_cfmakeraw
#define cfgetispeed win32_cfgetispeed
#define cfgetospeed win32_cfgetospeed
#define cfsetispeed win32_cfsetispeed
#define cfsetospeed win32_cfsetospeed
#define ioctl       win32_ioctl
#undef isatty
#define isatty      win32_isatty
#define ttyname     win32_ttyname

/* File operations */
int win32_open(const char *path, int flags, ...);
int win32_close(int fd);
ssize_t win32_read(int fd, void *buf, size_t count);
ssize_t win32_write(int fd, const void *buf, size_t count);
int win32_dup(int oldfd);
int win32_dup2(int oldfd, int newfd);
int win32_pipe(int pipefd[2]);
int win32_fcntl(int fd, int cmd, ...);
int win32_flock(int fd, int operation);
int win32_socketpair(int domain, int type, int protocol, int sv[2]);
int win32_connect(int s, const struct sockaddr *name, int namelen);
int win32_chdir(const char *path);
char *win32_getcwd(char *buf, size_t size);
int win32_unlink(const char *path);
int win32_mkdir(const char *path, mode_t mode);
int win32_rmdir(const char *path);
int win32_access(const char *path, int mode);
int win32_socket(int domain, int type, int protocol);
int win32_bind(int fd, const struct sockaddr *name, int namelen);
int win32_listen(int fd, int backlog);
int win32_accept(int fd, struct sockaddr *addr, int *addrlen);
int win32_setsockopt(int fd, int level, int optname, const void *optval, int optlen);
int win32_getsockopt(int fd, int level, int optname, void *optval, int *optlen);
int win32_getsockname(int fd, struct sockaddr *name, int *namelen);
int win32_getpeername(int fd, struct sockaddr *name, int *namelen);
ssize_t win32_send(int fd, const void *buf, size_t len, int flags);
ssize_t win32_recv(int fd, void *buf, size_t len, int flags);
int win32_reverse_lookup(SOCKET s);
struct event;
void win32_event_set(struct event *ev, int fd, short events, void (*cb)(int, short, void *), void *arg);

/* Use CRT versions for basic I/O, override where needed */
#define pipe(fds) win32_pipe(fds)
#define socketpair(d,t,p,sv) win32_socketpair(d,t,p,sv)
#define fcntl(fd,cmd,...) win32_fcntl(fd,cmd,__VA_ARGS__)

#ifdef connect
#undef connect
#endif
#define connect(s,name,len) win32_connect(s,name,len)

#ifdef close
#undef close
#endif
#define close(fd) win32_close(fd)
#define read(fd, b, c) win32_read(fd, b, c)
#define write(fd, b, c) win32_write(fd, b, c)

#define socket(d, t, p) win32_socket(d, t, p)
#define bind(f, n, l) win32_bind(f, n, l)
#define listen(f, b) win32_listen(f, b)
#define accept(f, a, l) win32_accept(f, a, l)
#define setsockopt(f, l, n, v, len) win32_setsockopt(f, l, n, v, len)
#define getsockopt(f, l, n, v, len) win32_getsockopt(f, l, n, v, len)
#define getsockname(f, n, l) win32_getsockname(f, n, l)
#define getpeername(f, n, l) win32_getpeername(f, n, l)
#define send(f, b, l, fl) win32_send(f, b, l, fl)
#define recv(f, b, l, fl) win32_recv(f, b, l, fl)

#undef event_set
#define event_set(ev, fd, events, cb, arg) win32_event_set(ev, fd, events, (void(*)(int, short, void*))cb, arg)

/* bufferevent wrapper to translate mapped fd to real SOCKET for libevent */
struct bufferevent;
typedef void (*evbuffercb)(struct bufferevent *, void *);
typedef void (*everrorcb)(struct bufferevent *, short, void *);
struct bufferevent *win32_bufferevent_new(int fd, evbuffercb readcb, evbuffercb writecb, everrorcb errorcb, void *cbarg);
#undef bufferevent_new
#define bufferevent_new(fd, rcb, wcb, ecb, arg) win32_bufferevent_new(fd, rcb, wcb, ecb, arg)


void win32_log(const char *fmt, ...);

#define flock(fd, op) (0)
#define unlink      win32_unlink
#define rmdir       win32_rmdir

#ifndef F_OK
#define F_OK 0
#endif
#ifndef W_OK
#define W_OK 2
#endif
#ifndef R_OK
#define R_OK 4
#endif
#ifndef X_OK
#define X_OK 0 /* Map X_OK to existence on Windows */
#endif

/* libgen.h compatibility */
char *win32_basename(char *path);
char *win32_dirname(char *path);
#define basename win32_basename
#define dirname  win32_dirname

/* pwd.h compatibility */
struct passwd {
    char *pw_name;
    char *pw_passwd;
    uid_t pw_uid;
    gid_t pw_gid;
    char *pw_gecos;
    char *pw_dir;
    char *pw_shell;
};
struct passwd *win32_getpwuid(uid_t uid);
struct passwd *win32_getpwnam(const char *name);
#define getpwuid win32_getpwuid
#define getpwnam win32_getpwnam

/* Network */
int win32_getpeereid(int sock, uid_t *euid, gid_t *egid);
#define getpeereid win32_getpeereid

/* Users */
uid_t win32_getuid(void);
uid_t win32_geteuid(void);
gid_t win32_getgid(void);
gid_t win32_getegid(void);
pid_t win32_getpid(void);
pid_t win32_getppid(void);
pid_t win32_getpgid(pid_t pid);
pid_t win32_getpgrp(void);
int   win32_setpgid(pid_t pid, pid_t pgid);
pid_t win32_setsid(void);

#define getuid   win32_getuid
#define geteuid  win32_geteuid
#define getgid   win32_getgid
#define getegid  win32_getegid
#undef getpid
#define getpid   win32_getpid
#define getppid  win32_getppid
#define getpgid  win32_getpgid
#define getpgrp  win32_getpgrp
#define setpgid  win32_setpgid
#define setsid   win32_setsid

/* Time */
int win32_clock_gettime(int clock_id, struct timespec *tp);
int win32_nanosleep(const struct timespec *req, struct timespec *rem);
int win32_usleep(unsigned int usec);
unsigned int win32_sleep(unsigned int seconds);

#define clock_gettime win32_clock_gettime
#define nanosleep     win32_nanosleep
#define usleep        win32_usleep
#undef sleep
#define sleep         win32_sleep

/* Misc */
void win32_closefrom(int lowfd);
int  win32_daemon(int nochdir, int noclose);
int  win32_setenv(const char *name, const char *value, int overwrite);
int  win32_unsetenv(const char *name);

#define closefrom win32_closefrom
#define daemon    win32_daemon

/* Environment path translation */
#undef getenv
#define getenv win32_getenv_path
char *win32_getenv_path(const char *name);
char *win32_translate_path(const char *path);
char *win32_translate_socket_path(const char *path);
pid_t win32_spawn_process(int *, struct tty *, struct winsize *, int, char **, struct environ *);

/* Network helper renaming */
#undef gethostname
#define gethostname win32_gethostname
int win32_gethostname(char *name, size_t len);

/*
 * ConPTY API wrapper
 */
typedef struct win32_pty win32_pty_t;

win32_pty_t *win32_pty_open(int cols, int rows);
void win32_pty_close(win32_pty_t *pty);
int win32_pty_resize(win32_pty_t *pty, int cols, int rows);
void win32_pty_get_size(win32_pty_t *pty, int *cols, int *rows);
DWORD win32_pty_get_child_pid(int fd);
void win32_pty_register(int fd, win32_pty_t *pty);
void win32_pty_teardown_for_respawn(int fd);
win32_pty_t *win32_pty_lookup(int fd);
HANDLE win32_pty_get_input_handle(win32_pty_t *pty);
HANDLE win32_pty_get_output_handle(win32_pty_t *pty);
HPCON win32_pty_get_console(win32_pty_t *pty);
HANDLE win32_pty_get_job(win32_pty_t *pty);
COORD  win32_pty_get_coord(win32_pty_t *pty);
int win32_pty_spawn(win32_pty_t *pty, const char *cmdline, void *env, PROCESS_INFORMATION *pi);

/*
 * Event/signal system
 */
typedef void (*win32_signal_handler_t)(int);

int win32_signal_init(void);
void win32_signal_cleanup(void);
int win32_signal_register(int sig, win32_signal_handler_t handler);
void win32_signal_process(void);

/* Process monitoring */
int win32_process_watch(HANDLE hProcess, void (*callback)(void *), void *arg);
void win32_process_unwatch(HANDLE hProcess);

/* Job execution (GAP-09: if-shell / run-shell) */
int   win32_job_init(void);
pid_t win32_job_run(const char *cmd, const char *shell);
pid_t win32_job_run_io(const char *cmd, const char *shell, int *out_fd);
pid_t win32_job_run_pty(const char *cmd, const char *shell, int sx, int sy,
    int *out_fd);
int   win32_job_register_exit(HANDLE hProcess, DWORD pid);
int   win32_pipe_pane_open(const char *cmd, int *out_pipe_fd);
int   win32_pipe_pane_open_io(const char *cmd, int in, int out, int *out_pipe_fd);

/* Clipboard integration (GAP-03) */
void  win32_clipboard_set(const char *buf, size_t len);
char *win32_clipboard_get(size_t *out_len);

/* Job control for process-tree suspend/resume/kill (PERM-02) */
HANDLE win32_jobctl_create(DWORD pid);
int    win32_jobctl_suspend(HANDLE hJob, DWORD child_pid);
int    win32_jobctl_resume(HANDLE hJob, DWORD child_pid, HPCON hPC, COORD size);
void   win32_jobctl_kill(HANDLE hJob);
void   win32_jobctl_close(HANDLE hJob);
void   win32_jobctl_send_signal(DWORD root_pid);

/* Peer identity verification (PERM-03: SO_PEERCRED equivalent) */
int  win32_check_peer_is_owner(int fd);

/* Session logging (PERM-04: utmp/wtmp shim) */
void win32_session_log_init(void);
void win32_session_log_open(int pane_id, long pid, const char *shell);
void win32_session_log_close(long pid);
void win32_session_log_cleanup(void);

/*
 * Socket helpers for libevent integration
 */
SOCKET win32_get_real_socket(int fd);
int win32_get_real_socket_maybe(int fd, SOCKET *s);
int win32_socket_init(void);
void win32_socket_cleanup(void);
int win32_socket_set_nonblocking(SOCKET sock);
int win32_socket_pair(SOCKET sv[2]);
SOCKET win32_from_map(int fd);
/* Fake fd offset: socket fds start at this value, above CRT fd space. */
#define WINSOCK_FD_OFFSET 2000

/*
 * Error handling
 */
void win32_perror(const char *msg);
char *win32_strerror(int errnum);

/* wcwidth — Unicode column-width (compat/win32-wcwidth.c) */
int wcwidth(wchar_t wc);

#ifdef __cplusplus
}
#endif

#define OCRNL  0000010
#define ONLRET 0000100
#define TCOFLUSH 1

#define mkdir(p, m) _mkdir(p)
#include <direct.h>

#ifdef __cplusplus
extern "C" {
#endif
struct tm *localtime(const time_t *timep);
struct tm *gmtime(const time_t *timep);
size_t win32_strftime_safe(char *s, size_t maxsize, const char *format, const struct tm *tm);
#undef strftime
#define strftime win32_strftime_safe
#ifdef __cplusplus
}
#endif

/* Undefine conflicting Winsock macros globally */
#ifdef msg_control
#undef msg_control
#endif
#ifdef msg_iov
#undef msg_iov
#endif
#ifdef msg_flags
#undef msg_flags
#endif
#ifdef msg_name
#undef msg_name
pid_t win32_spawn_process(int *, struct tty *, struct winsize *, int, char **, struct environ *);
#endif
#ifdef msg_namelen
#undef msg_namelen
#endif
#ifdef msg_iovlen
#undef msg_iovlen
#endif
#ifdef msg_controllen
#undef msg_controllen
#endif

#endif /* COMPAT_WIN32_H */
