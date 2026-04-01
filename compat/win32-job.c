/*
 * win32-job.c - Windows implementation of tmux job execution (GAP-09)
 *
 * Two execution paths:
 *
 * Fast Path (exit-code only, updatecb == NULL && completecb == NULL or
 * completecb doesn't need stdout — used by if-shell):
 *   1. CreateProcessA (no pipes, STARTF_USESTDHANDLES → INVALID_HANDLE_VALUE)
 *   2. RegisterWaitForSingleObject → sends PID over self-pipe on exit
 *   3. win32_job_done_cb() calls job_check_died() which fires completecb
 *      directly (job->event == NULL fast-path in job.c)
 *
 * Full I/O Bridge Path (stdout captured — used by run-shell, pipe-pane):
 *   1. CreatePipe(hReadPipe, hWritePipe) for child stdout/stderr
 *   2. CreateProcessA with hStdOutput = hStdError = hWritePipe,
 *      bInheritHandles = TRUE
 *   3. Close hWritePipe in parent immediately after CreateProcessA
 *   4. win32_socketpair() → [read_sock, write_sock]:
 *      - Bridge thread: ReadFile(hReadPipe) → win32_send(write_sock);
 *        exits naturally when ReadFile returns ERROR_BROKEN_PIPE (child exits
 *        and we close hReadPipe); then closes write_sock (EOF to bufferevent)
 *      - job->fd = read_sock; job->event = bufferevent_new(read_sock, ...)
 *   5. RegisterWaitForSingleObject stores exit code in ctx, bridge thread
 *      sends PID to self-pipe after draining (ensuring all output is in the
 *      bufferevent before job_check_died fires)
 *   6. job_check_died() sets job->state = JOB_DEAD; bufferevent EOF fires
 *      job_error_callback() → completecb() (standard POSIX flow)
 *
 * This avoids fork() + socketpair() + execl() — none of which exist on Win32.
 */

#include "tmux.h"
#include <stdlib.h>
#include <string.h>
#include <process.h>

/*
 * Self-pipe socket pair for marshalling job-exit events back to libevent.
 * write_sock: written by NT thread pool callback (any thread)
 * read_sock:  monitored by libevent on the main thread (EV_READ | EV_PERSIST)
 *
 * Protocol: each message is a struct win32_job_msg (8 bytes).
 */
struct win32_job_msg {
    DWORD pid;
    DWORD exit_code;
};

static int           job_pipe_read  = -1;  /* mapped fd, libevent side */
static int           job_pipe_write = -1;  /* mapped fd, thread pool side */
static struct event *job_pipe_event = NULL;
static int           job_pipe_initialized = 0;

/*
 * Per-job context stored by the thread pool wait registration.
 * Uses the PID as the lookup key — avoids raw job pointer in thread callback.
 *
 * For the Full I/O Bridge Path, exit_code is set by win32_job_wait_cb before
 * the bridge thread sends the PID over the self-pipe, ensuring the exit code
 * is available when win32_job_done_cb calls job_check_died.
 */
struct win32_job_ctx {
    DWORD   pid;
    HANDLE  hProcess;
    HANDLE  hWait;
    /* I/O bridge fields (NULL/INVALID in fast path) */
    HANDLE  hReadPipe;   /* read end of child stdout pipe; bridge thread owns */
    int     write_sock;  /* write end of loopback pair; bridge thread closes */
    volatile LONG exit_code; /* set by wait callback, read by bridge thread */
    volatile LONG exited;    /* 1 after exit_code is written */
};

/* Forward declaration */
static void CALLBACK win32_job_wait_cb(PVOID param, BOOLEAN fired);
static void          win32_job_done_cb(int fd, short events, void *arg);
static unsigned __stdcall win32_job_bridge_thread(void *param);

/*
 * win32_job_init - initialize the self-pipe and register libevent event.
 *
 * Must be called once on the main thread after the libevent event_base
 * is active (i.e. after proc_start() in server_child_main).
 */
int
win32_job_init(void)
{
    int sv[2];

    if (job_pipe_initialized)
        return 0;

    if (win32_socketpair(AF_INET, SOCK_STREAM, 0, sv) != 0) {
        win32_log("win32_job_init: socketpair failed: %d\n", WSAGetLastError());
        return -1;
    }

    job_pipe_read  = sv[0];
    job_pipe_write = sv[1];

    /* Set the read socket non-blocking so the drain loop terminates cleanly */
    {
        SOCKET rs = win32_get_real_socket(job_pipe_read);
        u_long mode = 1;
        ioctlsocket(rs, FIONBIO, &mode);
    }

    job_pipe_event = xmalloc(sizeof *job_pipe_event);
    event_set(job_pipe_event, job_pipe_read, EV_READ | EV_PERSIST,
        win32_job_done_cb, NULL);
    if (event_add(job_pipe_event, NULL) != 0) {
        win32_log("win32_job_init: event_add failed\n");
        free(job_pipe_event);
        job_pipe_event = NULL;
        return -1;
    }

    job_pipe_initialized = 1;
    win32_log("win32_job_init: self-pipe ready read_fd=%d write_fd=%d\n",
        job_pipe_read, job_pipe_write);
    return 0;
}

/*
 * win32_job_run - Windows replacement for job_run()'s fork+exec path.
 *
 * Spawns `cmd` via cmd.exe /C, registers a thread pool wait, and returns
 * the PID.  The job struct is populated by the caller in job_run() after
 * this function returns.
 *
 * Returns the child PID on success, -1 on failure.
 */
pid_t
win32_job_run(const char *cmd, const char *shell)
{
    STARTUPINFOA        si;
    PROCESS_INFORMATION pi;
    char               *cmdline;
    size_t              len;
    struct win32_job_ctx *ctx;
    const char         *sh;

    if (!job_pipe_initialized) {
        win32_log("win32_job_run: self-pipe not initialized\n");
        return -1;
    }

    /* Determine shell: prefer caller-supplied, fall back to COMSPEC */
    if (shell != NULL && shell[0] != '\0' && shell[0] != '/')
        sh = shell;
    else {
        sh = getenv("COMSPEC");
        if (sh == NULL)
            sh = "cmd.exe";
    }

    /* Build: shell /C "cmd" */
    len = strlen(sh) + strlen(" /C ") + strlen(cmd) + 3;
    cmdline = xmalloc(len);
    snprintf(cmdline, len, "%s /C %s", sh, cmd);

    win32_log("win32_job_run: spawning: %s\n", cmdline);

    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    /* Redirect child stdin/stdout/stderr to NUL so it doesn't inherit our console */
    si.hStdInput  = INVALID_HANDLE_VALUE;
    si.hStdOutput = INVALID_HANDLE_VALUE;
    si.hStdError  = INVALID_HANDLE_VALUE;

    memset(&pi, 0, sizeof pi);

    if (!CreateProcessA(
            NULL,
            cmdline,
            NULL,           /* process security */
            NULL,           /* thread security */
            FALSE,          /* bInheritHandles - no pipes to pass */
            CREATE_NO_WINDOW,
            NULL,           /* inherit environment */
            NULL,           /* inherit cwd */
            &si,
            &pi)) {
        win32_log("win32_job_run: CreateProcessA failed: %lu\n", GetLastError());
        free(cmdline);
        return -1;
    }
    free(cmdline);
    CloseHandle(pi.hThread);

    win32_log("win32_job_run: child pid=%lu hProcess=%p\n",
        pi.dwProcessId, pi.hProcess);

    /* Allocate context for the thread pool callback */
    ctx = xmalloc(sizeof *ctx);
    ctx->pid       = pi.dwProcessId;
    ctx->hProcess  = pi.hProcess;
    ctx->hWait     = NULL;
    ctx->hReadPipe = INVALID_HANDLE_VALUE; /* signals fast path to wait_cb */
    ctx->write_sock = -1;
    ctx->exit_code  = 0;
    ctx->exited     = 0;

    if (!RegisterWaitForSingleObject(
            &ctx->hWait,
            pi.hProcess,
            win32_job_wait_cb,
            ctx,
            INFINITE,
            WT_EXECUTEONLYONCE)) {
        win32_log("win32_job_run: RegisterWaitForSingleObject failed: %lu\n",
            GetLastError());
        CloseHandle(pi.hProcess);
        free(ctx);
        return -1;
    }

    return (pid_t)pi.dwProcessId;
}

/*
 * win32_job_run_io - Full I/O Bridge Path.
 *
 * Same as win32_job_run but captures child stdout/stderr via a pipe +
 * bridge thread, making it available via a bufferevent (job->event).
 *
 * Returns the child PID on success and sets *out_fd to the read end of
 * the loopback socket pair (caller sets job->fd and job->event on this fd).
 * Returns -1 on failure.
 */
pid_t
win32_job_run_io(const char *cmd, const char *shell, int *out_fd)
{
    STARTUPINFOA        si;
    PROCESS_INFORMATION pi;
    HANDLE              hReadPipe  = INVALID_HANDLE_VALUE;
    HANDLE              hWritePipe = INVALID_HANDLE_VALUE;
    SECURITY_ATTRIBUTES sa;
    char               *cmdline;
    size_t              len;
    struct win32_job_ctx *ctx;
    const char         *sh;
    int                 sv[2];
    uintptr_t           tid;

    if (!job_pipe_initialized) {
        win32_log("win32_job_run_io: self-pipe not initialized\n");
        return -1;
    }

    /* Determine shell */
    if (shell != NULL && shell[0] != '\0' && shell[0] != '/')
        sh = shell;
    else {
        sh = getenv("COMSPEC");
        if (sh == NULL)
            sh = "cmd.exe";
    }

    len = strlen(sh) + strlen(" /C ") + strlen(cmd) + 3;
    cmdline = xmalloc(len);
    snprintf(cmdline, len, "%s /C %s", sh, cmd);

    /* Create inheritable pipe for child stdout/stderr */
    memset(&sa, 0, sizeof sa);
    sa.nLength              = sizeof sa;
    sa.bInheritHandle       = TRUE;
    sa.lpSecurityDescriptor = NULL;

    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        win32_log("win32_job_run_io: CreatePipe failed: %lu\n", GetLastError());
        free(cmdline);
        return -1;
    }
    /* Make the read end non-inheritable so child doesn't get it */
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    /* Loopback socket pair: bridge thread writes to sv[1], bufferevent reads sv[0] */
    if (win32_socketpair(AF_INET, SOCK_STREAM, 0, sv) != 0) {
        win32_log("win32_job_run_io: socketpair failed: %d\n", WSAGetLastError());
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
        free(cmdline);
        return -1;
    }

    win32_log("win32_job_run_io: spawning: %s\n", cmdline);

    memset(&si, 0, sizeof si);
    si.cb        = sizeof si;
    si.dwFlags   = STARTF_USESTDHANDLES;
    si.hStdInput = INVALID_HANDLE_VALUE;
    si.hStdOutput = hWritePipe;
    si.hStdError  = hWritePipe;

    memset(&pi, 0, sizeof pi);

    if (!CreateProcessA(
            NULL,
            cmdline,
            NULL, NULL,
            TRUE,           /* bInheritHandles — needed for hWritePipe */
            CREATE_NO_WINDOW,
            NULL, NULL,
            &si, &pi)) {
        win32_log("win32_job_run_io: CreateProcessA failed: %lu\n",
            GetLastError());
        free(cmdline);
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
        win32_close(sv[0]);
        win32_close(sv[1]);
        return -1;
    }
    free(cmdline);
    CloseHandle(pi.hThread);

    /*
     * Close hWritePipe in the parent immediately.  The child holds its own
     * copy via inheritance.  When the child exits its handle closes too,
     * causing ReadFile(hReadPipe) to return ERROR_BROKEN_PIPE.
     */
    CloseHandle(hWritePipe);

    win32_log("win32_job_run_io: child pid=%lu hProcess=%p\n",
        pi.dwProcessId, pi.hProcess);

    ctx = xmalloc(sizeof *ctx);
    ctx->pid        = pi.dwProcessId;
    ctx->hProcess   = pi.hProcess;
    ctx->hWait      = NULL;
    ctx->hReadPipe  = hReadPipe;
    ctx->write_sock = sv[1];
    ctx->exit_code  = 0;
    ctx->exited     = 0;

    if (!RegisterWaitForSingleObject(
            &ctx->hWait,
            pi.hProcess,
            win32_job_wait_cb,
            ctx,
            INFINITE,
            WT_EXECUTEONLYONCE)) {
        win32_log("win32_job_run_io: RegisterWaitForSingleObject failed: %lu\n",
            GetLastError());
        CloseHandle(pi.hProcess);
        CloseHandle(hReadPipe);
        win32_close(sv[0]);
        win32_close(sv[1]);
        free(ctx);
        return -1;
    }

    /* Launch bridge thread — it owns ctx, hReadPipe, sv[1] */
    tid = _beginthreadex(NULL, 0, win32_job_bridge_thread, ctx, 0, NULL);
    if (tid == 0) {
        win32_log("win32_job_run_io: _beginthreadex failed: %d\n", errno);
        UnregisterWait(ctx->hWait);
        CloseHandle(pi.hProcess);
        CloseHandle(hReadPipe);
        win32_close(sv[0]);
        win32_close(sv[1]);
        free(ctx);
        return -1;
    }
    CloseHandle((HANDLE)tid);

    *out_fd = sv[0];
    return (pid_t)pi.dwProcessId;
}

/*
 * win32_job_register_exit - register a process for self-pipe exit notification.
 *
 * Called by win32_job_run_pty (in win32-pty.c) to hook the ConPTY child into
 * the same job_check_died/completecb dispatch path as win32_job_run_io.
 * The ctx is allocated here and freed in win32_job_wait_cb.
 *
 * Returns 0 on success, -1 on failure.
 */
int
win32_job_register_exit(HANDLE hProcess, DWORD pid)
{
    struct win32_job_ctx *ctx;
    HANDLE               hOwned;

    if (!job_pipe_initialized)
        return -1;

    /*
     * Open a fresh handle rather than sharing the caller's hProcess.
     * win32_job_run_pty passes the same pi.hProcess to pty_process_exited
     * (which closes it on child exit) — reusing that handle here would
     * leave a dangling reference after pty_process_exited fires.
     */
    hOwned = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE, pid);
    if (hOwned == NULL) {
        win32_log("win32_job_register_exit: OpenProcess failed: %lu\n",
            GetLastError());
        return -1;
    }

    ctx = xmalloc(sizeof *ctx);
    ctx->pid        = pid;
    ctx->hProcess   = hOwned;
    ctx->hWait      = NULL;
    ctx->hReadPipe  = INVALID_HANDLE_VALUE;
    ctx->write_sock = (SOCKET)-1;
    ctx->exit_code  = 0;
    ctx->exited     = 0;

    if (!RegisterWaitForSingleObject(&ctx->hWait, hOwned,
            win32_job_wait_cb, ctx, INFINITE, WT_EXECUTEONLYONCE)) {
        win32_log("win32_job_register_exit: RegisterWaitForSingleObject failed: %lu\n",
            GetLastError());
        CloseHandle(hOwned);
        free(ctx);
        return -1;
    }
    win32_log("win32_job_register_exit: registered pid=%lu\n", pid);
    return 0;
}

/*
 * win32_job_wait_cb - NT thread pool callback, fires when child exits.
 *
 * WARNING: Runs on a thread pool worker thread, NOT the libevent thread.
 * Must not touch any tmux data structures directly.
 *
 * Fast path: sends PID immediately over the self-pipe, then frees ctx.
 * I/O bridge path: stores exit code in ctx->exit_code and sets ctx->exited.
 *   The bridge thread will send the PID after draining the pipe.
 */
static void CALLBACK
win32_job_wait_cb(PVOID param, BOOLEAN fired)
{
    struct win32_job_ctx *ctx = param;
    DWORD pid = ctx->pid;
    DWORD exitCode = 1;

    (void)fired;

    win32_log("win32_job_wait_cb: child pid=%lu exited\n", pid);

    GetExitCodeProcess(ctx->hProcess, &exitCode);
    UnregisterWait(ctx->hWait);
    CloseHandle(ctx->hProcess);
    ctx->hProcess = NULL;

    if (ctx->hReadPipe == INVALID_HANDLE_VALUE) {
        /*
         * Fast path: no bridge thread — send msg directly and free ctx.
         */
        struct win32_job_msg msg = { pid, exitCode };
        win32_send(job_pipe_write, (char *)&msg, sizeof msg, 0);
        free(ctx);
    } else {
        /*
         * I/O bridge path: store exit code; bridge thread will send PID
         * after the pipe drains (ensuring all output reaches the bufferevent
         * before job_check_died fires).
         */
        InterlockedExchange(&ctx->exit_code, (LONG)exitCode);
        InterlockedExchange(&ctx->exited, 1);
        /* Bridge thread is blocking on ReadFile; closing the child's write
         * end (hWritePipe, already closed by spawn code) caused EOF.  The
         * bridge thread will detect ERROR_BROKEN_PIPE naturally and proceed
         * to send the PID.  Nothing else to do here. */
    }
}

/*
 * win32_job_bridge_thread - bridge thread for Full I/O Bridge Path.
 *
 * Runs on its own thread (not libevent thread).  Reads child stdout from
 * hReadPipe and writes to write_sock.  When the pipe reports EOF
 * (ERROR_BROKEN_PIPE — child exited and write end was closed), sends the PID
 * over the self-pipe to trigger job_check_died on the main thread, then
 * closes write_sock (which triggers bufferevent EOF → job_error_callback).
 *
 * Ownership: this thread owns hReadPipe and write_sock.  It frees ctx.
 */
static unsigned __stdcall
win32_job_bridge_thread(void *param)
{
    struct win32_job_ctx *ctx = param;
    DWORD  pid       = ctx->pid;
    HANDLE hReadPipe = ctx->hReadPipe;
    int    write_fd  = ctx->write_sock;
    char   buf[4096];
    DWORD  nread;

    win32_log("win32_job_bridge_thread: started for pid=%lu\n", pid);

    for (;;) {
        if (!ReadFile(hReadPipe, buf, sizeof buf, &nread, NULL)) {
            DWORD err = GetLastError();
            if (err == ERROR_BROKEN_PIPE || err == ERROR_NO_DATA)
                break; /* child exited, pipe drained */
            win32_log("win32_job_bridge_thread: ReadFile error=%lu, stopping\n",
                err);
            break;
        }
        if (nread == 0)
            break;
        win32_send(write_fd, buf, (int)nread, 0);
    }

    win32_log("win32_job_bridge_thread: pipe drained for pid=%lu\n", pid);

    CloseHandle(hReadPipe);

    /*
     * Wait for win32_job_wait_cb to store the exit code in ctx.
     * In normal operation the child is already dead by the time
     * ERROR_BROKEN_PIPE fires, so this spin is rarely more than 1 iteration.
     */
    while (!ctx->exited)
        Sleep(1);

    /* Send msg to self-pipe — this wakes win32_job_done_cb on main thread */
    {
        struct win32_job_msg msg = { pid, (DWORD)ctx->exit_code };
        win32_send(job_pipe_write, (char *)&msg, sizeof msg, 0);
    }

    /*
     * Close write_sock — this signals EOF to the bufferevent read end,
     * which fires job_error_callback → completecb on the main thread.
     * Do this AFTER sending the PID so job->state is JOB_DEAD before
     * job_error_callback checks it.
     */
    win32_close(write_fd);

    free(ctx);
    return 0;
}

/*
 * win32_job_done_cb - libevent read callback on the self-pipe.
 *
 * Runs on the main libevent thread.  Drains all pending PIDs and calls
 * job_check_died() for each one, which fires cmd_if_shell_callback etc.
 */
static void
win32_job_done_cb(int fd, short events, void *arg)
{
    struct win32_job_msg msg;
    int  status;

    (void)events;
    (void)arg;

    for (;;) {
        int r = win32_recv(fd, (char *)&msg, sizeof msg, 0);
        if (r != (int)sizeof msg)
            break;

        /* Encode as POSIX waitpid status: exit code in bits 15:8 */
        status = (int)(msg.exit_code & 0xff) << 8;

        win32_log("win32_job_done_cb: pid=%lu exitCode=%lu status=0x%x\n",
            msg.pid, msg.exit_code, status);

        job_check_died((pid_t)msg.pid, status);
    }
    win32_log("win32_job_done_cb: recv loop done\n");
}

/*
 * pipe-pane egress bridge context.
 * Owned by the egress bridge thread; freed by that thread on exit.
 */
struct win32_pipe_pane_ctx {
    HANDLE  hWritePipe;  /* write end of child stdin pipe */
    int     read_sock;   /* sv[1]: recv data here, forward to child stdin */
};

/*
 * win32_pipe_pane_egress_thread - forwards data from socket to child stdin.
 *
 * Runs on its own thread.  Blocks on recv(read_sock); each chunk received is
 * written to hWritePipe (child stdin).  Exits when recv returns 0 or an
 * error (the socket was closed by cmd_pipe_pane_error_callback or
 * window_pane_free calling close(wp->pipe_fd) which closes sv[0], causing
 * sv[1] to see EOF on the next recv).
 */
static unsigned __stdcall
win32_pipe_pane_egress_thread(void *param)
{
    struct win32_pipe_pane_ctx *ctx = param;
    HANDLE  hWritePipe = ctx->hWritePipe;
    int     read_sock  = ctx->read_sock;
    char    buf[4096];
    int     nrecv;
    DWORD   nwritten;

    win32_log("win32_pipe_pane_egress_thread: started\n");
    free(ctx);

    for (;;) {
        nrecv = win32_recv(read_sock, buf, sizeof buf, 0);
        if (nrecv <= 0)
            break;
        if (!WriteFile(hWritePipe, buf, (DWORD)nrecv, &nwritten, NULL))
            break;
    }

    win32_log("win32_pipe_pane_egress_thread: exiting\n");
    CloseHandle(hWritePipe);
    win32_close(read_sock);
    return 0;
}

/*
 * win32_pipe_pane_open - Windows replacement for the socketpair()+fork() block
 * in cmd_pipe_pane_exec.
 *
 * Spawns `cmd` via cmd.exe /C with its stdin connected to the write end of an
 * anonymous pipe.  Creates a loopback socketpair; sets *out_pipe_fd to sv[0]
 * (this becomes wp->pipe_fd / wp->pipe_event).  Starts an egress bridge thread
 * that reads from sv[1] and writes to hWritePipe (child stdin).
 *
 * Data flow (output direction, -O, default):
 *   window_pane_read_callback
 *     → bufferevent_write(wp->pipe_event, new_data)
 *     → libevent drains sv[0] (EV_WRITE)
 *     → egress thread recv(sv[1]) → WriteFile(hWritePipe) → child stdin
 *
 * Returns 0 on success and sets *out_pipe_fd.
 * Returns -1 on failure.
 */
int
win32_pipe_pane_open(const char *cmd, int *out_pipe_fd)
{
    HANDLE              hReadPipe  = INVALID_HANDLE_VALUE;
    HANDLE              hWritePipe = INVALID_HANDLE_VALUE;
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOA        si;
    PROCESS_INFORMATION pi;
    char               *cmdline;
    size_t              len;
    int                 sv[2];
    const char         *sh;
    uintptr_t           tid;
    struct win32_pipe_pane_ctx *ctx;

    sh = getenv("COMSPEC");
    if (sh == NULL)
        sh = "cmd.exe";

    len = strlen(sh) + strlen(" /C ") + strlen(cmd) + 3;
    cmdline = xmalloc(len);
    snprintf(cmdline, len, "%s /C %s", sh, cmd);

    /* Create inheritable pipe for child stdin */
    memset(&sa, 0, sizeof sa);
    sa.nLength              = sizeof sa;
    sa.bInheritHandle       = TRUE;
    sa.lpSecurityDescriptor = NULL;

    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        win32_log("win32_pipe_pane_open: CreatePipe failed: %lu\n",
            GetLastError());
        free(cmdline);
        return -1;
    }
    /* Make write end non-inheritable — only parent/bridge thread uses it */
    SetHandleInformation(hWritePipe, HANDLE_FLAG_INHERIT, 0);

    /* Loopback socket pair: sv[0] = wp->pipe_fd, sv[1] = egress bridge */
    if (win32_socketpair(AF_INET, SOCK_STREAM, 0, sv) != 0) {
        win32_log("win32_pipe_pane_open: socketpair failed: %d\n",
            WSAGetLastError());
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
        free(cmdline);
        return -1;
    }

    win32_log("win32_pipe_pane_open: spawning: %s\n", cmdline);

    memset(&si, 0, sizeof si);
    si.cb         = sizeof si;
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = hReadPipe;   /* child reads its stdin from here */
    si.hStdOutput = INVALID_HANDLE_VALUE;
    si.hStdError  = INVALID_HANDLE_VALUE;

    memset(&pi, 0, sizeof pi);

    if (!CreateProcessA(
            NULL, cmdline,
            NULL, NULL,
            TRUE,            /* bInheritHandles — needed for hReadPipe */
            CREATE_NO_WINDOW,
            NULL, NULL,
            &si, &pi)) {
        win32_log("win32_pipe_pane_open: CreateProcessA failed: %lu\n",
            GetLastError());
        free(cmdline);
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
        win32_close(sv[0]);
        win32_close(sv[1]);
        return -1;
    }
    free(cmdline);

    /*
     * Close the read end in the parent — child inherits it as stdin.
     * Close process/thread handles; we don't monitor this process.
     */
    CloseHandle(hReadPipe);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    win32_log("win32_pipe_pane_open: child pid=%lu\n", pi.dwProcessId);

    ctx = xmalloc(sizeof *ctx);
    ctx->hWritePipe = hWritePipe;
    ctx->read_sock  = sv[1];

    tid = _beginthreadex(NULL, 0, win32_pipe_pane_egress_thread, ctx, 0, NULL);
    if (tid == 0) {
        win32_log("win32_pipe_pane_open: _beginthreadex failed: %d\n", errno);
        CloseHandle(hWritePipe);
        win32_close(sv[0]);
        win32_close(sv[1]);
        free(ctx);
        return -1;
    }
    CloseHandle((HANDLE)tid);

    *out_pipe_fd = sv[0];
    return 0;
}

/*
 * win32_pipe_pane_ingress_thread - forwards child stdout to the socket.
 *
 * Mirrors win32_pipe_pane_egress_thread but in the opposite direction:
 * ReadFile(hReadPipe) → send(write_sock).
 * When ReadFile returns ERROR_BROKEN_PIPE (child exited and we closed
 * hWritePipe), the thread closes write_sock, which causes libevent to fire
 * the EV_READ EOF path → cmd_pipe_pane_error_callback → cleanup.
 */
struct win32_pipe_pane_ingress_ctx {
    HANDLE  hReadPipe;   /* read end of child stdout pipe */
    int     write_sock;  /* sv[1]: send data here → bufferevent EV_READ on sv[0] */
};

static unsigned __stdcall
win32_pipe_pane_ingress_thread(void *param)
{
    struct win32_pipe_pane_ingress_ctx *ctx = param;
    HANDLE  hReadPipe  = ctx->hReadPipe;
    int     write_sock = ctx->write_sock;
    char    buf[4096];
    DWORD   nread;

    win32_log("win32_pipe_pane_ingress_thread: started\n");
    free(ctx);

    for (;;) {
        if (!ReadFile(hReadPipe, buf, sizeof buf, &nread, NULL)) {
            DWORD err = GetLastError();
            if (err != ERROR_BROKEN_PIPE)
                win32_log("win32_pipe_pane_ingress_thread: ReadFile failed: %lu\n", err);
            break;
        }
        if (nread == 0)
            break;
        if (win32_send(write_sock, buf, (int)nread, 0) <= 0)
            break;
    }

    win32_log("win32_pipe_pane_ingress_thread: exiting\n");
    CloseHandle(hReadPipe);
    win32_close(write_sock);
    return 0;
}

/*
 * win32_pipe_pane_open_io - Windows implementation of pipe-pane -IO (both).
 *
 * Like win32_pipe_pane_open but also captures child stdout and feeds it back
 * into the pane via the bufferevent EV_READ path.
 *
 * Data flow (-I direction, child stdout → pane):
 *   child stdout → hReadPipe
 *     ingress thread: ReadFile(hReadPipe) → send(sv[1])
 *     libevent EV_READ on sv[0] → cmd_pipe_pane_read_callback
 *       → bufferevent_write(wp->event, data)
 *       → win32_bridge_in on wp->fd → WriteFile(hPipeIn) → pane ConPTY input
 *
 * Data flow (-O direction, pane output → child stdin):
 *   window_pane_read_callback → bufferevent_write(wp->pipe_event, data)
 *   libevent EV_WRITE on sv[0] → egress thread recv(sv[1]) → WriteFile(hWriteStdin)
 *
 * When out=0 (pure -I), hWriteStdin is /dev/null and the egress thread is
 * not started; EV_WRITE is not enabled on the bufferevent.
 *
 * Returns 0 on success, -1 on failure.
 */
int
win32_pipe_pane_open_io(const char *cmd, int in, int out, int *out_pipe_fd)
{
    HANDLE              hStdinRead   = INVALID_HANDLE_VALUE;
    HANDLE              hStdinWrite  = INVALID_HANDLE_VALUE;
    HANDLE              hStdoutRead  = INVALID_HANDLE_VALUE;
    HANDLE              hStdoutWrite = INVALID_HANDLE_VALUE;
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOA        si;
    PROCESS_INFORMATION pi;
    char               *cmdline;
    size_t              len;
    int                 sv[2];
    const char         *sh;
    uintptr_t           tid;

    sh = getenv("COMSPEC");
    if (sh == NULL)
        sh = "cmd.exe";

    len = strlen(sh) + strlen(" /C ") + strlen(cmd) + 3;
    cmdline = xmalloc(len);
    snprintf(cmdline, len, "%s /C %s", sh, cmd);

    memset(&sa, 0, sizeof sa);
    sa.nLength              = sizeof sa;
    sa.bInheritHandle       = TRUE;
    sa.lpSecurityDescriptor = NULL;

    /* stdin pipe (child reads) — always created; write end is /dev/null when out=0 */
    if (!CreatePipe(&hStdinRead, &hStdinWrite, &sa, 0)) {
        win32_log("win32_pipe_pane_open_io: CreatePipe(stdin) failed: %lu\n", GetLastError());
        free(cmdline);
        return -1;
    }
    SetHandleInformation(hStdinWrite, HANDLE_FLAG_INHERIT, 0);

    /* stdout pipe (child writes) — always created; read end drives ingress thread */
    if (!CreatePipe(&hStdoutRead, &hStdoutWrite, &sa, 0)) {
        win32_log("win32_pipe_pane_open_io: CreatePipe(stdout) failed: %lu\n", GetLastError());
        CloseHandle(hStdinRead);
        CloseHandle(hStdinWrite);
        free(cmdline);
        return -1;
    }
    SetHandleInformation(hStdoutRead, HANDLE_FLAG_INHERIT, 0);

    /* Loopback socket pair: sv[0] = wp->pipe_fd, sv[1] = bridge threads */
    if (win32_socketpair(AF_INET, SOCK_STREAM, 0, sv) != 0) {
        win32_log("win32_pipe_pane_open_io: socketpair failed: %d\n", WSAGetLastError());
        CloseHandle(hStdinRead);  CloseHandle(hStdinWrite);
        CloseHandle(hStdoutRead); CloseHandle(hStdoutWrite);
        free(cmdline);
        return -1;
    }

    win32_log("win32_pipe_pane_open_io: spawning (in=%d out=%d): %s\n", in, out, cmdline);

    memset(&si, 0, sizeof si);
    si.cb         = sizeof si;
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = out ? hStdinRead   : INVALID_HANDLE_VALUE;
    si.hStdOutput = in  ? hStdoutWrite : INVALID_HANDLE_VALUE;
    si.hStdError  = INVALID_HANDLE_VALUE;

    memset(&pi, 0, sizeof pi);

    if (!CreateProcessA(NULL, cmdline, NULL, NULL, TRUE,
            CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        win32_log("win32_pipe_pane_open_io: CreateProcessA failed: %lu\n", GetLastError());
        free(cmdline);
        CloseHandle(hStdinRead);  CloseHandle(hStdinWrite);
        CloseHandle(hStdoutRead); CloseHandle(hStdoutWrite);
        win32_close(sv[0]); win32_close(sv[1]);
        return -1;
    }
    free(cmdline);
    win32_log("win32_pipe_pane_open_io: child pid=%lu\n", pi.dwProcessId);

    /* Close child-side ends in parent; child inherits them via bInheritHandles */
    CloseHandle(hStdinRead);
    CloseHandle(hStdoutWrite);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    /* Ingress thread: ReadFile(hStdoutRead) → send(sv[1]) → EV_READ on sv[0] */
    if (in) {
        struct win32_pipe_pane_ingress_ctx *ictx = xmalloc(sizeof *ictx);
        ictx->hReadPipe  = hStdoutRead;
        ictx->write_sock = sv[1];
        tid = _beginthreadex(NULL, 0, win32_pipe_pane_ingress_thread, ictx, 0, NULL);
        if (tid == 0) {
            win32_log("win32_pipe_pane_open_io: _beginthreadex(ingress) failed: %d\n", errno);
            CloseHandle(hStdoutRead);
            CloseHandle(hStdinWrite);
            win32_close(sv[0]); win32_close(sv[1]);
            free(ictx);
            return -1;
        }
        CloseHandle((HANDLE)tid);
    } else {
        CloseHandle(hStdoutRead);
    }

    /* Egress thread: recv(sv[1]) → WriteFile(hStdinWrite) → child stdin */
    if (out) {
        struct win32_pipe_pane_ctx *ectx = xmalloc(sizeof *ectx);
        ectx->hWritePipe = hStdinWrite;
        ectx->read_sock  = sv[1];
        tid = _beginthreadex(NULL, 0, win32_pipe_pane_egress_thread, ectx, 0, NULL);
        if (tid == 0) {
            win32_log("win32_pipe_pane_open_io: _beginthreadex(egress) failed: %d\n", errno);
            CloseHandle(hStdinWrite);
            win32_close(sv[0]); win32_close(sv[1]);
            free(ectx);
            return -1;
        }
        CloseHandle((HANDLE)tid);
    } else {
        CloseHandle(hStdinWrite);
    }

    /*
     * sv[1] stays in the fd map so the bridge threads can call win32_send/
     * win32_recv(sv[1], ...) via win32_from_map.  The last thread to exit
     * calls win32_close(sv[1]) which removes it from the map.  If both
     * threads are running (-IO), the first close makes the second thread's
     * next recv/send fail, causing it to exit cleanly.
     */

    *out_pipe_fd = sv[0];
    return 0;
}
