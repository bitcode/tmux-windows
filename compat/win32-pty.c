/*
 * win32-pty.c - ConPTY wrapper for tmux Windows port
 *
 * This file provides a pseudo-terminal implementation using
 * Windows ConPTY (Pseudo Console) API, available in Windows 10 1809+.
 */

#include "tmux.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * PTY fd map: maps a socket fd (sv[0]) to its win32_pty_t* so that
 * win32_ioctl(TIOCSWINSZ) can resize the ConPTY.
 */
#define WIN32_PTY_MAP_SIZE 256
static struct { int fd; win32_pty_t *pty; } pty_fd_map[WIN32_PTY_MAP_SIZE];
static int pty_fd_map_count = 0;

void
win32_pty_register(int fd, win32_pty_t *pty)
{
    int i;
    /* Evict any stale entry for this fd (e.g. fd reused before old process
     * exits and pty_process_exited fires its deregister). */
    for (i = 0; i < pty_fd_map_count; i++) {
        if (pty_fd_map[i].fd == fd) {
            win32_log("win32_pty_register: evicting stale entry for fd=%d\n",
                fd);
            pty_fd_map[i] = pty_fd_map[--pty_fd_map_count];
            break;
        }
    }
    if (pty_fd_map_count < WIN32_PTY_MAP_SIZE) {
        pty_fd_map[pty_fd_map_count].fd  = fd;
        pty_fd_map[pty_fd_map_count].pty = pty;
        pty_fd_map_count++;
    }
}

win32_pty_t *
win32_pty_lookup(int fd)
{
    int i;
    for (i = 0; i < pty_fd_map_count; i++) {
        if (pty_fd_map[i].fd == fd)
            return pty_fd_map[i].pty;
    }
    return NULL;
}

/*
 * PTY structure - wraps ConPTY handles
 */
struct win32_pty {
    HPCON hPC;              /* Pseudo console handle */
    HANDLE hPipeIn;         /* Pipe for input to child (we write) */
    HANDLE hPipeOut;        /* Pipe for output from child (we read) */
    HANDLE hPipeInChild;    /* Child side of input pipe */
    HANDLE hPipeOutChild;   /* Child side of output pipe */
    COORD size;             /* Current size */
    DWORD child_pid;        /* PID of spawned child process */
    HANDLE hJob;            /* Job Object for process-tree control (PERM-02) */
    HANDLE hProcess;        /* Child process handle (kept for cleanup) */
    HANDLE hWaitHandle;     /* RegisterWaitForSingleObject handle */
    int    master_fd;       /* sv[0] fd registered in pty_fd_map */
};

static void
win32_pty_deregister(int fd)
{
    int i;
    for (i = 0; i < pty_fd_map_count; i++) {
        if (pty_fd_map[i].fd == fd) {
            pty_fd_map[i] = pty_fd_map[--pty_fd_map_count];
            return;
        }
    }
}

/*
 * Cleanup callback fired by RegisterWaitForSingleObject when the child exits.
 * Runs on a Windows thread pool thread — must be async-safe.
 */
static void CALLBACK
pty_process_exited(void *param, BOOLEAN TimerOrWaitFired)
{
    win32_pty_t *pty = (win32_pty_t *)param;
    (void)TimerOrWaitFired;

    win32_log("pty_process_exited: pid=%lu, hPC=%p\n", pty->child_pid, pty->hPC);

    /* Unregister this wait — use NULL event, no blocking */
    if (pty->hWaitHandle != NULL) {
        UnregisterWait(pty->hWaitHandle);
        pty->hWaitHandle = NULL;
    }

    /* Close the pseudo console — this signals the bridge threads to exit */
    if (pty->hPC != NULL) {
        ClosePseudoConsole(pty->hPC);
        pty->hPC = NULL;
    }

    /* Close the process handle */
    if (pty->hProcess != NULL) {
        CloseHandle(pty->hProcess);
        pty->hProcess = NULL;
    }

    /* Release the Job Object */
    win32_jobctl_close(pty->hJob);
    pty->hJob = NULL;

    /* Remove from the fd map */
    win32_pty_deregister(pty->master_fd);

    free(pty);
}

HANDLE
win32_pty_get_job(win32_pty_t *pty)
{
    return pty != NULL ? pty->hJob : NULL;
}

COORD
win32_pty_get_coord(win32_pty_t *pty)
{
    COORD zero = {0, 0};
    return pty != NULL ? pty->size : zero;
}

/*
 * Create a new pseudo-terminal
 */
win32_pty_t *
win32_pty_open(int cols, int rows)
{
    win32_pty_t *pty;
    HRESULT hr;
    HANDLE hPipeInRead = INVALID_HANDLE_VALUE;
    HANDLE hPipeInWrite = INVALID_HANDLE_VALUE;
    HANDLE hPipeOutRead = INVALID_HANDLE_VALUE;
    HANDLE hPipeOutWrite = INVALID_HANDLE_VALUE;
    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };

    pty = calloc(1, sizeof(*pty));
    if (pty == NULL)
        return NULL;
    
#ifdef PLATFORM_WINDOWS
    win32_log("win32_pty_open: entry, cols=%d, rows=%d\n", cols, rows);
#endif

    pty->size.X = (SHORT)cols;
    pty->size.Y = (SHORT)rows;

    /*
     * Create pipes for communication with the pseudo console.
     * The child writes to hPipeOutWrite, we read from hPipeOutRead.
     * We write to hPipeInWrite, the child reads from hPipeInRead.
     */
#ifdef PLATFORM_WINDOWS
    win32_log("win32_pty_open: calling CreatePipe 1\n");
#endif
    if (!CreatePipe(&hPipeInRead, &hPipeInWrite, &sa, 0)) {
#ifdef PLATFORM_WINDOWS
        win32_log("win32_pty_open: CreatePipe 1 failed\n");
#endif
        goto fail;
    }
#ifdef PLATFORM_WINDOWS
    win32_log("win32_pty_open: calling CreatePipe 2\n");
#endif
    if (!CreatePipe(&hPipeOutRead, &hPipeOutWrite, &sa, 0)) {
#ifdef PLATFORM_WINDOWS
        win32_log("win32_pty_open: CreatePipe 2 failed\n");
#endif
        CloseHandle(hPipeInRead);
        CloseHandle(hPipeInWrite);
        goto fail;
    }

    /* Ensure the write handle to the pipe for STDIN is not inherited */
    if (!SetHandleInformation(hPipeInWrite, HANDLE_FLAG_INHERIT, 0)) {
        goto fail_close_pipes;
    }
    if (!SetHandleInformation(hPipeOutRead, HANDLE_FLAG_INHERIT, 0)) {
        goto fail_close_pipes;
    }

    /*
     * Create the pseudo console
     */
#ifdef PLATFORM_WINDOWS
    win32_log("win32_pty_open: calling CreatePseudoConsole\n");
#endif
    hr = CreatePseudoConsole(pty->size, hPipeInRead, hPipeOutWrite, 0, &pty->hPC);
    if (FAILED(hr)) {
#ifdef PLATFORM_WINDOWS
        win32_log("win32_pty_open: CreatePseudoConsole failed, hr=%x\n", hr);
#endif
        goto fail_close_pipes;
    }

    /*
     * Store handles:
     * - We keep the write end of input pipe (to send data to child)
     * - We keep the read end of output pipe (to receive data from child)
     * - Child sides are used in spawn and then closed
     */
    pty->hPipeIn = hPipeInWrite;
    pty->hPipeOut = hPipeOutRead;
    pty->hPipeInChild = hPipeInRead;
    pty->hPipeOutChild = hPipeOutWrite;

    pty->hPipeOutChild = hPipeOutWrite;

#ifdef PLATFORM_WINDOWS
    win32_log("win32_pty_open: success, pty=%p\n", pty);
#endif
    return pty;

fail_close_pipes:
    CloseHandle(hPipeInRead);
    CloseHandle(hPipeInWrite);
    CloseHandle(hPipeOutRead);
    CloseHandle(hPipeOutWrite);
fail:
    free(pty);
    return NULL;
}

/*
 * Close and free a pseudo-terminal
 */
void
win32_pty_close(win32_pty_t *pty)
{
    if (pty == NULL)
        return;

    if (pty->hPC != NULL)
        ClosePseudoConsole(pty->hPC);

    if (pty->hPipeIn != INVALID_HANDLE_VALUE)
        CloseHandle(pty->hPipeIn);
    if (pty->hPipeOut != INVALID_HANDLE_VALUE)
        CloseHandle(pty->hPipeOut);
    if (pty->hPipeInChild != INVALID_HANDLE_VALUE)
        CloseHandle(pty->hPipeInChild);
    if (pty->hPipeOutChild != INVALID_HANDLE_VALUE)
        CloseHandle(pty->hPipeOutChild);

    free(pty);
}

/*
 * Get current PTY dimensions
 */
DWORD
win32_pty_get_child_pid(int fd)
{
    win32_pty_t *pty = win32_pty_lookup(fd);
    return pty ? pty->child_pid : 0;
}

void
win32_pty_get_size(win32_pty_t *pty, int *cols, int *rows)
{
    if (pty) {
        if (cols) *cols = pty->size.X;
        if (rows) *rows = pty->size.Y;
    } else {
        if (cols) *cols = 80;
        if (rows) *rows = 24;
    }
}

/*
 * Resize a pseudo-terminal
 */
int
win32_pty_resize(win32_pty_t *pty, int cols, int rows)
{
    HRESULT hr;

    if (pty == NULL || pty->hPC == NULL)
        return -1;

    pty->size.X = (SHORT)cols;
    pty->size.Y = (SHORT)rows;

    hr = ResizePseudoConsole(pty->hPC, pty->size);
    return SUCCEEDED(hr) ? 0 : -1;
}

/*
 * Get the handle for reading from the PTY (child's output)
 */
HANDLE
win32_pty_get_input_handle(win32_pty_t *pty)
{
    return pty ? pty->hPipeIn : INVALID_HANDLE_VALUE;
}

/*
 * Get the handle for writing to the PTY (child's input)
 */
HANDLE
win32_pty_get_output_handle(win32_pty_t *pty)
{
    return pty ? pty->hPipeOut : INVALID_HANDLE_VALUE;
}

/*
 * Get the pseudo console handle
 */
HPCON
win32_pty_get_console(win32_pty_t *pty)
{
    return pty ? pty->hPC : NULL;
}

/*
 * Spawn a process attached to the pseudo-terminal
 */
int
win32_pty_spawn(win32_pty_t *pty, const char *cmdline, void *env, PROCESS_INFORMATION *pi)
{
    STARTUPINFOEXW si;
    SIZE_T attrListSize = 0;
    BOOL success = FALSE;
    wchar_t *wcmdline = NULL;
    int wcmdlen;
    wchar_t *wenv = NULL;

    if (pty == NULL || cmdline == NULL || pi == NULL)
        return -1;

#ifdef PLATFORM_WINDOWS
    win32_log("win32_pty_spawn: entry, cmdline=%s\n", cmdline);
#endif

    memset(&si, 0, sizeof(si));
    si.StartupInfo.cb = sizeof(STARTUPINFOEXW);

    /*
     * Initialize the attribute list with the pseudo console attribute
     */
    InitializeProcThreadAttributeList(NULL, 1, 0, &attrListSize);
    si.lpAttributeList = HeapAlloc(GetProcessHeap(), 0, attrListSize);
    if (si.lpAttributeList == NULL)
        return -1;

    if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attrListSize)) {
        goto cleanup;
    }

    if (!UpdateProcThreadAttribute(
            si.lpAttributeList,
            0,
            PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
            pty->hPC,
            sizeof(HPCON),
            NULL,
            NULL)) {
        goto cleanup;
    }

    /*
     * Convert command line to wide string
     */
    wcmdlen = MultiByteToWideChar(CP_UTF8, 0, cmdline, -1, NULL, 0);
    if (wcmdlen <= 0)
        goto cleanup;

    wcmdline = HeapAlloc(GetProcessHeap(), 0, wcmdlen * sizeof(wchar_t));
    if (wcmdline == NULL)
        goto cleanup;

    MultiByteToWideChar(CP_UTF8, 0, cmdline, -1, wcmdline, wcmdlen);

    /* Convert env block if present (it's char*, needs wchar_t*) 
       Actually CreateProcessW needs wide environment if CREATE_UNICODE_ENVIRONMENT is set. 
       If we consistently use A functions, we can use char* env.
       But we use CreateProcessW. So `env` (char*) must be converted to `wchar_t*` OR we use CreateProcessA?
       CreatePseudoConsole requires CreateProcess? No, it just gives handles.
       If we use CreateProcessA, we might have encoding issues.
       tmux uses UTF-8 internally. 
       So we should convert env block to Wide. 
       Or... implementing env conversion is tedious.
       Standard `env` passed from `win32_spawn_process` is `char*` (UTF-8 keys/values).
       So we need utility to convert `char*` block to `wchar_t*` block.
       
       Simplification: Pass NULL for environment for now unless critical.
       tmux sets default environment. If omitted, it inherits parent environment which IS WRONG for tmux.
       But fixing env conversion is "Phase 3.5"?
       Task says "Implement forkpty emulation using ConPTY". 
       I will set `env` to NULL in CreateProcessW call for now, but keeping the argument in function signature.
    */

    /*
     * Create the process
     */
    success = CreateProcessW(
        NULL,                           /* Application name */
        wcmdline,                       /* Command line */
        NULL,                           /* Process attributes */
        NULL,                           /* Thread attributes */
        FALSE,                          /* Inherit handles */
        EXTENDED_STARTUPINFO_PRESENT,   /* Creation flags */
        env,                           /* Environment */
        NULL,                           /* Current directory */
        &si.StartupInfo,                /* Startup info */
        pi                              /* Process information */
    );

#ifdef PLATFORM_WINDOWS
    win32_log("win32_pty_spawn: CreateProcessW result=%d\n", success);
#endif

    /*
     * Close the child-side pipe handles now that the process is created
     */
    if (success) {
        if (pty->hPipeInChild != INVALID_HANDLE_VALUE) {
            CloseHandle(pty->hPipeInChild);
            pty->hPipeInChild = INVALID_HANDLE_VALUE;
        }
        if (pty->hPipeOutChild != INVALID_HANDLE_VALUE) {
            CloseHandle(pty->hPipeOutChild);
            pty->hPipeOutChild = INVALID_HANDLE_VALUE;
        }
    }

cleanup:
    if (wcmdline)
        HeapFree(GetProcessHeap(), 0, wcmdline);
    if (si.lpAttributeList) {
        DeleteProcThreadAttributeList(si.lpAttributeList);
        HeapFree(GetProcessHeap(), 0, si.lpAttributeList);
    }

    return success ? 0 : -1;
}

/*
 * forkpty replacement - creates PTY and spawns a shell
 *
 * This is the main compatibility function that replaces the Unix forkpty().
 * Unlike Unix, we cannot actually fork - instead we spawn a new process.
 *
 * Returns:
 *  - In the "parent" context: child PID (actually always returns > 0 on success)
 *  - On error: -1
 *
 * Note: The Unix forkpty() returns 0 in the child, but since we don't fork,
 * this function always runs in the parent context.
 */
pid_t
win32_forkpty(int *amaster, char *name, struct termios *termp, struct winsize *winp)
{
    win32_pty_t *pty;
    PROCESS_INFORMATION pi;
    const char *shell;
    int cols = 80, rows = 24;

    (void)termp;  /* termios not directly applicable */

    if (winp != NULL) {
        cols = winp->ws_col;
        rows = winp->ws_row;
    }

    pty = win32_pty_open(cols, rows);
    if (pty == NULL)
        return -1;

    /* Get shell from environment or use default */
    shell = getenv("COMSPEC");
    if (shell == NULL)
        shell = "cmd.exe";

    memset(&pi, 0, sizeof(pi));
    if (win32_pty_spawn(pty, shell, NULL, &pi) != 0) {
        win32_pty_close(pty);
        return -1;
    }

    /*
     * Return the output handle as the "master" fd
     * The caller will use this for I/O with the child
     *
     * Note: This is a HANDLE cast to int, which works on Windows
     * but the caller should be aware this isn't a real file descriptor
     */
    if (amaster != NULL) {
        /*
         * Convert the handle to a C file descriptor.
         * _open_osfhandle takes a C Runtime handle (intptr_t) and flags.
         * The handle is the Write end of the Output pipe (child writes to it? Wait.)
         * NO. win32_pty_get_output_handle returns hPipeOut.
         * hPipeOut is the READ end for the PARENT.
         * The child uses hPipeOutChild (Write).
         * So we open hPipeOut as read-only.
         */
        int fd = _open_osfhandle((intptr_t)win32_pty_get_output_handle(pty), _O_RDONLY);
        if (fd == -1) {
            win32_pty_close(pty);
            return -1;
        }
        *amaster = fd;
        
        /* 
         * Also define win32_spawn_process as a better alternative 
         * since forkpty cannot handle arguments.
         */
    }


    if (name != NULL) {
        /* No TTY name on Windows, return placeholder */
        strcpy(name, "conpty");
    }

    /*
     * Close the thread handle, keep the process handle
     * The process handle is needed for waitpid()
     */
    CloseHandle(pi.hThread);

    /* Return the process ID */
    return (pid_t)pi.dwProcessId;
}

#include <fcntl.h>
#include <io.h>
#include <process.h>

struct bridge_info {
    SOCKET s;
    HANDLE hPipe;
    HANDLE hPipeIn;
};
/*
 * Bridge thread: Socket -> Pipe (Input to PTY)
 */
unsigned __stdcall
win32_bridge_in(void *arg)
{
    struct bridge_info *info = arg;
    char buf[4096];
    int n;
    DWORD written;

    while (1) {
        n = recv(info->s, buf, sizeof(buf), 0);
        if (n <= 0) break;
        if (!WriteFile(info->hPipe, buf, n, &written, NULL)) break;
    }

    /* Shutdown pipe write side? generic pipes don't support shutdown like sockets */
    /* Just close handle? */
    /* Close handles handled by cleanup? */
    CloseHandle(info->hPipe);
    closesocket(info->s); /* Verify? Usually main thread closes s? No, this is s_bridge */
    free(info);
    return 0;
}

/*
 * Bridge thread: Pipe -> Socket (Output from PTY)
 *
 * This thread also handles the DSR (Device Status Report) handshake.
 * ConPTY sends \x1b[6n to query cursor position, and expects a response
 * in the format \x1b[<row>;<col>R on the input pipe. Without this response,
 * ConPTY may hang or stop processing input.
 */
unsigned __stdcall
win32_bridge_out(void *arg)
{
    struct bridge_info *info = arg;
    char buf[4096];
    DWORD n;
    static const char dsr_query[] = "\x1b[6n";
    static const char dsr_response[] = "\x1b[1;1R";

#ifdef PLATFORM_WINDOWS
    win32_log("win32_bridge_out: thread started, pipe=%p, socket=%llu, hPipeIn=%p\n", 
              info->hPipe, (unsigned long long)info->s, info->hPipeIn);
#endif

    while (1) {
        if (!ReadFile(info->hPipe, buf, sizeof(buf), &n, NULL)) {
#ifdef PLATFORM_WINDOWS
            DWORD rdErr = GetLastError();
            if (rdErr == ERROR_OPERATION_ABORTED) {
                /* CancelSynchronousIo was called (e.g. before NtSuspendProcess).
                 * The pipe is still valid — retry the read. */
                continue;
            }
            win32_log("win32_bridge_out: ReadFile failed, error=%lu\n", rdErr);
#endif
            break;
        }
        if (n == 0) {
#ifdef PLATFORM_WINDOWS
            win32_log("win32_bridge_out: ReadFile returned 0 bytes\n");
#endif
            break;
        }
#ifdef PLATFORM_WINDOWS
        win32_log("win32_bridge_out: read %lu bytes from pipe\n", n);
#endif

        /*
         * DSR Handshake Detection and Response
         *
         * Scan the buffer for \x1b[6n (DSR cursor position query).
         * If found, respond with cursor position to the input pipe.
         * We report (1,1) as the initial position.
         */
        char *dsr_pos;
        DWORD i = 0;
        while (i < n) {
            /* Search for DSR query starting at position i */
            dsr_pos = NULL;
            for (DWORD j = i; j + 3 < n; j++) {
                if (buf[j] == '\x1b' && buf[j+1] == '[' && buf[j+2] == '6' && buf[j+3] == 'n') {
                    dsr_pos = &buf[j];
                    break;
                }
            }
            /* Also check the last 3 bytes for partial matches at boundary */
            if (dsr_pos == NULL && n >= 4 && i == 0) {
                if (buf[n-4] == '\x1b' && buf[n-3] == '[' && buf[n-2] == '6' && buf[n-1] == 'n') {
                    dsr_pos = &buf[n-4];
                }
            }

            if (dsr_pos != NULL) {
                DWORD dsr_offset = (DWORD)(dsr_pos - buf);
                
                /* Send everything before the DSR query to the socket */
                if (dsr_offset > i) {
                    int sent = send(info->s, buf + i, dsr_offset - i, 0);
                    if (sent <= 0) {
#ifdef PLATFORM_WINDOWS
                        win32_log("win32_bridge_out: send (pre-DSR) failed, error=%d\n", WSAGetLastError());
#endif
                        goto cleanup;
                    }
#ifdef PLATFORM_WINDOWS
                    win32_log("win32_bridge_out: sent %d bytes (before DSR) to socket\n", sent);
#endif
                }

                /* Respond to DSR query by writing cursor position to input pipe */
                if (info->hPipeIn != INVALID_HANDLE_VALUE && info->hPipeIn != NULL) {
                    DWORD written;
                    if (WriteFile(info->hPipeIn, dsr_response, sizeof(dsr_response) - 1, &written, NULL)) {
#ifdef PLATFORM_WINDOWS
                        win32_log("win32_bridge_out: DSR response sent (%lu bytes) - cursor position 1;1\n", written);
#endif
                    } else {
#ifdef PLATFORM_WINDOWS
                        win32_log("win32_bridge_out: DSR response WriteFile failed, error=%lu\n", GetLastError());
#endif
                    }
                } else {
#ifdef PLATFORM_WINDOWS
                    win32_log("win32_bridge_out: DSR detected but hPipeIn invalid, cannot respond\n");
#endif
                }

                /* Skip past the DSR query (4 bytes: ESC [ 6 n) */
                i = dsr_offset + 4;
            } else {
                /* No more DSR queries, send the rest of the buffer */
                if (i < n) {
                    int sent = send(info->s, buf + i, n - i, 0);
                    if (sent <= 0) {
#ifdef PLATFORM_WINDOWS
                        win32_log("win32_bridge_out: send failed, error=%d\n", WSAGetLastError());
#endif
                        goto cleanup;
                    }
#ifdef PLATFORM_WINDOWS
                    win32_log("win32_bridge_out: sent %d bytes to socket\n", sent);
#endif
                }
                break;  /* Done processing this buffer */
            }
        }
    }

cleanup:
#ifdef PLATFORM_WINDOWS
    win32_log("win32_bridge_out: thread exiting\n");
#endif
    CloseHandle(info->hPipe);
    if (info->hPipeIn != INVALID_HANDLE_VALUE && info->hPipeIn != NULL) {
        CloseHandle(info->hPipeIn);
    }
    closesocket(info->s);
    free(info);
    return 0;
}


/*
 * Spawn a process with PTY support and Socket Bridge
 */
pid_t
win32_spawn_process(int *master_fd, struct tty *tty, struct winsize *ws,
    int argc, char **argv, struct environ *env)
{
    win32_pty_t *pty;
    PROCESS_INFORMATION pi;
    int cols = 80, rows = 24;
    size_t cmdlen = 0;
    char *cmdline, *ptr;
    int i;
    struct environ_entry *envent;
    char *envblock = NULL;
    size_t envsize = 0;
    int sv[2]; 
    HANDLE hInDup, hOutDup;
    struct bridge_info *bi_in, *bi_out;

    if (ws != NULL) {
        cols = ws->ws_col;
        rows = ws->ws_row;
    } else if (tty != NULL) {
        cols = tty->sx;
        rows = tty->sy;
    }

    pty = win32_pty_open(cols, rows);
    if (pty == NULL) {
#ifdef PLATFORM_WINDOWS
        win32_log("win32_spawn_process: win32_pty_open failed\n");
#endif
        return -1;
    }

    /* Build command line - if argc is 0, use the shell from options or environment */
    if (argc == 0) {
        const char *shell = NULL;
        /* Check default-shell option - only use if it's a Windows path (not Unix /bin/sh default) */
        if (global_s_options != NULL) {
            const char *opt = options_get_string(global_s_options, "default-shell");
            /* Only use if non-empty and not a Unix-style path (tmux default is /bin/sh) */
            if (opt != NULL && opt[0] != '\0' && opt[0] != '/')
                shell = opt;
        }
        /* Fall back to COMSPEC, then cmd.exe */
        if (shell == NULL || shell[0] == '\0')
            shell = getenv("COMSPEC");
        if (shell == NULL || shell[0] == '\0')
            shell = "cmd.exe";
        cmdlen = strlen(shell) + 1;
        cmdline = xmalloc(cmdlen);
        strcpy(cmdline, shell);
#ifdef PLATFORM_WINDOWS
        win32_log("win32_spawn_process: argc=0, using shell: '%s'\n", cmdline);
#endif
    } else {
        for (i = 0; i < argc; i++)
            cmdlen += strlen(argv[i]) + 3; /* quotes + space */
        cmdline = xmalloc(cmdlen + 1);
        ptr = cmdline;
        for (i = 0; i < argc; i++) {
            if (i > 0)
                *ptr++ = ' ';
            *ptr++ = '"';
            strcpy(ptr, argv[i]);
            ptr += strlen(argv[i]);
            *ptr++ = '"';
        }
        *ptr = '\0';
    }

#ifdef PLATFORM_WINDOWS
    win32_log("win32_spawn_process: cmdline built: '%s'\n", cmdline);
#endif


    /* Build environment block */
    if (env != NULL) {
#ifdef PLATFORM_WINDOWS
        win32_log("win32_spawn_process: building env block\n");
#endif
        for (envent = environ_first(env); envent != NULL; envent = environ_next(envent)) {
            if (envent->value == NULL)
                continue;
            envsize += strlen(envent->name) + 1 + strlen(envent->value) + 1;
        }
        envsize++;
        envblock = xmalloc(envsize);
        ptr = envblock;
        for (envent = environ_first(env); envent != NULL; envent = environ_next(envent)) {
            if (envent->value == NULL)
                continue;
            int l = snprintf(ptr, envsize - (ptr - envblock), "%s=%s", envent->name, envent->value);
            ptr += l + 1;
        }
        *ptr = '\0';
    }

    memset(&pi, 0, sizeof(pi));
#ifdef PLATFORM_WINDOWS
    win32_log("win32_spawn_process: calling win32_pty_spawn\n");
#endif
    if (win32_pty_spawn(pty, cmdline, (void*)envblock, &pi) != 0) {
#ifdef PLATFORM_WINDOWS
        win32_log("win32_spawn_process: win32_pty_spawn failed\n");
#endif
        free(cmdline);
        if (envblock) free(envblock);
        win32_pty_close(pty);
        return -1;
    }
    win32_session_log_open(0, (long)pi.dwProcessId, cmdline);
    free(cmdline);
    if (envblock) free(envblock);

    /*
     * Create socketpair for bridge
     */
    if (win32_socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
#ifdef PLATFORM_WINDOWS
        win32_log("win32_spawn_process: win32_socketpair failed\n");
#endif
        win32_pty_close(pty);
        return -1;
    }
#ifdef PLATFORM_WINDOWS
        win32_log("win32_spawn_process: socketpair sv[0]=%d, sv[1]=%d\n", sv[0], sv[1]);
#endif

    /*
     * Launch threads
     * 
     * IMPORTANT: sv[0] and sv[1] are mapped file descriptors from win32_add_to_map(),
     * NOT raw SOCKET handles. We must use win32_get_real_socket() to get the
     * actual SOCKET handles that Winsock functions expect.
     * 
     * Both bridge threads share the same socket handle for sv[1] (the "bridge side").
     * The socket is closed when BOTH threads exit (last one to call closesocket wins).
     * Note: DuplicateHandle does NOT work for Winsock sockets per Microsoft docs.
     */
    SOCKET bridge_socket = win32_get_real_socket(sv[1]);
#ifdef PLATFORM_WINDOWS
    win32_log("win32_spawn_process: bridge_socket=%llu (from fd=%d)\n", (unsigned long long)bridge_socket, sv[1]);
    win32_log("win32_spawn_process: pty->hPipeIn=%p, pty->hPipeOut=%p\n", pty->hPipeIn, pty->hPipeOut);
#endif

    bi_in = xmalloc(sizeof(*bi_in));
    bi_in->s = bridge_socket;
    /* pty->hPipeIn is WRITE end of pipe to child. */
    if (!DuplicateHandle(GetCurrentProcess(), pty->hPipeIn, GetCurrentProcess(), &hInDup, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
#ifdef PLATFORM_WINDOWS
        win32_log("win32_spawn_process: DuplicateHandle hPipeIn FAILED, error=%lu\n", GetLastError());
#endif
        hInDup = INVALID_HANDLE_VALUE;
    }
    bi_in->hPipe = hInDup;
#ifdef PLATFORM_WINDOWS
    win32_log("win32_spawn_process: bi_in->hPipe=%p (dup of hPipeIn)\n", bi_in->hPipe);
#endif
    bi_in->hPipeIn = NULL;  /* Input bridge doesn't need to send DSR responses */
    
    bi_out = xmalloc(sizeof(*bi_out));
    bi_out->s = bridge_socket;  /* Both threads share the same socket */
    if (!DuplicateHandle(GetCurrentProcess(), pty->hPipeOut, GetCurrentProcess(), &hOutDup, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
#ifdef PLATFORM_WINDOWS
        win32_log("win32_spawn_process: DuplicateHandle hPipeOut FAILED, error=%lu\n", GetLastError());
#endif
        hOutDup = INVALID_HANDLE_VALUE;
    }
    bi_out->hPipe = hOutDup;

    /* Duplicate hPipeIn for bi_out to send DSR responses */
    HANDLE hInDupForOut;
    if (!DuplicateHandle(GetCurrentProcess(), pty->hPipeIn, GetCurrentProcess(), &hInDupForOut, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
#ifdef PLATFORM_WINDOWS
        win32_log("win32_spawn_process: DuplicateHandle hPipeIn for DSR FAILED, error=%lu\n", GetLastError());
#endif
        hInDupForOut = INVALID_HANDLE_VALUE;
    }
    bi_out->hPipeIn = hInDupForOut;
#ifdef PLATFORM_WINDOWS
    win32_log("win32_spawn_process: bi_out->hPipe=%p (dup of hPipeOut), bi_out->hPipeIn=%p (for DSR)\n", bi_out->hPipe, bi_out->hPipeIn);
#endif

    _beginthreadex(NULL, 0, win32_bridge_in, bi_in, 0, NULL);
    _beginthreadex(NULL, 0, win32_bridge_out, bi_out, 0, NULL);
#ifdef PLATFORM_WINDOWS
    win32_log("win32_spawn_process: threads started\n");
#endif


    /* 
     * NOTE: We do NOT close the bridge socket here anymore.
     * The bridge threads now own it and will close it when they exit.
     * We also need to remove sv[1] from our fd map since the threads own the socket now.
     */
    /* Remove from map but don't close the socket - threads own it */
    win32_remove_from_map(sv[1]);

    /*
     * IMPORTANT: Do NOT call win32_pty_close here!
     * ClosePseudoConsole terminates the child process immediately.
     *
     * Per EchoCon sample and Microsoft docs, the child-side pipe handles
     * (hPipeInChild, hPipeOutChild) can be closed after CreatePseudoConsole
     * because they're duplicated into ConHost. But we must keep hPC alive
     * until the child process exits.
     *
     * GAP-15: PTY lifetime is now managed by pty_process_exited(), which fires
     * via RegisterWaitForSingleObject when the child exits.  It calls
     * ClosePseudoConsole, closes the Job Object, removes from pty_fd_map, and
     * frees the pty struct.  No manual cleanup is needed here.
     */
    if (pty->hPipeInChild != INVALID_HANDLE_VALUE) {
        CloseHandle(pty->hPipeInChild);
        pty->hPipeInChild = INVALID_HANDLE_VALUE;
    }
    if (pty->hPipeOutChild != INVALID_HANDLE_VALUE) {
        CloseHandle(pty->hPipeOutChild);
        pty->hPipeOutChild = INVALID_HANDLE_VALUE;
    }
    /* 
     * Also close our original pipe handles since we duplicated them for threads.
     * The threads have their own copies via DuplicateHandle.
     */
    if (pty->hPipeIn != INVALID_HANDLE_VALUE) {
        CloseHandle(pty->hPipeIn);
        pty->hPipeIn = INVALID_HANDLE_VALUE;
    }
    if (pty->hPipeOut != INVALID_HANDLE_VALUE) {
        CloseHandle(pty->hPipeOut);
        pty->hPipeOut = INVALID_HANDLE_VALUE;
    }
    /* Keep pty->hPC alive! We intentionally leak the PTY struct for now. */
#ifdef PLATFORM_WINDOWS
    win32_log("win32_spawn_process: PTY handles closed, hPC kept alive\n");
#endif

    *master_fd = sv[0]; /* Return master side socket */

    /* Store child PID and register pty so win32_ioctl(TIOCSWINSZ) can resize it */
    pty->child_pid = pi.dwProcessId;
    pty->master_fd = sv[0];

    /* Assign child to a Job Object for process-tree suspend/kill (PERM-02) */
    pty->hJob = win32_jobctl_create(pi.dwProcessId);

    /*
     * GAP-15: Keep process handle for cleanup; register a wait callback so
     * pty_process_exited fires when the child exits.  This ensures HPCON and
     * the Job Object are released without leaking handles or threads.
     */
    pty->hProcess = pi.hProcess;   /* do NOT CloseHandle here */
    pty->hWaitHandle = NULL;
    if (!RegisterWaitForSingleObject(&pty->hWaitHandle, pty->hProcess,
            pty_process_exited, pty, INFINITE, WT_EXECUTEONLYONCE)) {
        win32_log("win32_spawn_process: RegisterWaitForSingleObject failed: %lu\n",
            GetLastError());
        /* Non-fatal: PTY will leak on exit, but basic functionality is unaffected */
        pty->hWaitHandle = NULL;
    }

    win32_pty_register(sv[0], pty);

    CloseHandle(pi.hThread);
    return (pid_t)pi.dwProcessId;


}

pid_t
win32_fork(void)
{
    errno = ENOSYS;
    return -1;
}

/*
 * win32_pty_teardown_for_respawn - synchronous teardown before respawn.
 *
 * Called from spawn_pane() SPAWN_RESPAWN path immediately before
 * close(sc->wp0->fd).  Cancels the RegisterWaitForSingleObject callback so
 * pty_process_exited() cannot fire after we return and accidentally
 * deregister the new pty entry when the old fd number is recycled.
 *
 * If the pty is not found (pty_process_exited already ran), this is a no-op.
 */
void
win32_pty_teardown_for_respawn(int fd)
{
    win32_pty_t *pty = win32_pty_lookup(fd);
    if (pty == NULL) {
        /* pty_process_exited already ran — nothing to do */
        win32_log("win32_pty_teardown_for_respawn: fd=%d not found, already cleaned up\n", fd);
        return;
    }

    win32_log("win32_pty_teardown_for_respawn: fd=%d, pid=%lu\n", fd, pty->child_pid);

    /*
     * Cancel the wait registration synchronously.
     * UnregisterWaitEx with INVALID_HANDLE_VALUE blocks until any
     * in-progress callback completes, guaranteeing pty_process_exited
     * will not run after we return.
     */
    if (pty->hWaitHandle != NULL) {
        UnregisterWaitEx(pty->hWaitHandle, INVALID_HANDLE_VALUE);
        pty->hWaitHandle = NULL;
    }

    /* Close ConPTY — signals bridge threads to stop reading */
    if (pty->hPC != NULL) {
        ClosePseudoConsole(pty->hPC);
        pty->hPC = NULL;
    }

    /* Close process handle */
    if (pty->hProcess != NULL) {
        CloseHandle(pty->hProcess);
        pty->hProcess = NULL;
    }

    /* Release Job Object */
    win32_jobctl_close(pty->hJob);
    pty->hJob = NULL;

    /* Remove from fd map so the new spawn can register sv[0] */
    win32_pty_deregister(fd);

    free(pty);
}

/* -------------------------------------------------------------------------
 * win32_job_run_pty - ConPTY path for JOB_PTY jobs (display-popup).
 *
 * Creates a ConPTY of size |sx|x|sy|, spawns |cmd| (via |shell| /C cmd),
 * bridges ConPTY output → socket (for bufferevent/input_parse_screen) and
 * socket → ConPTY input (for keystrokes from bufferevent_write).
 * Returns child PID and sets *out_fd to sv[0] (the libevent side).
 * Child exit is reported via win32_job_register_exit → self-pipe path.
 * ---------------------------------------------------------------------- */
pid_t
win32_job_run_pty(const char *cmd, const char *shell, int sx, int sy,
    int *out_fd)
{
    win32_pty_t        *pty;
    PROCESS_INFORMATION pi;
    char               *cmdline;
    size_t              len;
    const char         *sh;
    int                 sv[2];
    HANDLE              hInDup, hOutDup, hInDupForOut;
    struct bridge_info *bi_in, *bi_out;
    SOCKET              bridge_socket;

    /* Determine shell */
    if (shell != NULL && shell[0] != '\0' && shell[0] != '/')
        sh = shell;
    else {
        sh = getenv("COMSPEC");
        if (sh == NULL)
            sh = "cmd.exe";
    }

    if (cmd != NULL && cmd[0] != '\0') {
        len = strlen(sh) + strlen(" /C ") + strlen(cmd) + 3;
        cmdline = xmalloc(len);
        snprintf(cmdline, len, "%s /C %s", sh, cmd);
    } else {
        cmdline = xstrdup(sh);
    }

    pty = win32_pty_open(sx, sy);
    if (pty == NULL) {
        win32_log("win32_job_run_pty: win32_pty_open failed\n");
        free(cmdline);
        return -1;
    }

    memset(&pi, 0, sizeof pi);
    if (win32_pty_spawn(pty, cmdline, NULL, &pi) != 0) {
        win32_log("win32_job_run_pty: win32_pty_spawn failed\n");
        free(cmdline);
        win32_pty_close(pty);
        return -1;
    }
    win32_log("win32_job_run_pty: child pid=%lu\n", pi.dwProcessId);
    free(cmdline);

    /* Socketpair: sv[0] = libevent/bufferevent side; sv[1] = bridge side */
    if (win32_socketpair(AF_INET, SOCK_STREAM, 0, sv) != 0) {
        win32_log("win32_job_run_pty: socketpair failed: %d\n", WSAGetLastError());
        win32_pty_close(pty);
        return -1;
    }
    bridge_socket = win32_get_real_socket(sv[1]);

    /* Ingress: socket → hPipeIn (keystrokes to child) */
    bi_in = xmalloc(sizeof *bi_in);
    if (!DuplicateHandle(GetCurrentProcess(), pty->hPipeIn, GetCurrentProcess(),
            &hInDup, 0, FALSE, DUPLICATE_SAME_ACCESS))
        hInDup = INVALID_HANDLE_VALUE;
    bi_in->s      = bridge_socket;
    bi_in->hPipe  = hInDup;
    bi_in->hPipeIn = NULL;

    /* Egress: hPipeOut → socket (child output to bufferevent) */
    bi_out = xmalloc(sizeof *bi_out);
    if (!DuplicateHandle(GetCurrentProcess(), pty->hPipeOut, GetCurrentProcess(),
            &hOutDup, 0, FALSE, DUPLICATE_SAME_ACCESS))
        hOutDup = INVALID_HANDLE_VALUE;
    if (!DuplicateHandle(GetCurrentProcess(), pty->hPipeIn, GetCurrentProcess(),
            &hInDupForOut, 0, FALSE, DUPLICATE_SAME_ACCESS))
        hInDupForOut = INVALID_HANDLE_VALUE;
    bi_out->s      = bridge_socket;
    bi_out->hPipe  = hOutDup;
    bi_out->hPipeIn = hInDupForOut;  /* DSR handshake */

    _beginthreadex(NULL, 0, win32_bridge_in,  bi_in,  0, NULL);
    _beginthreadex(NULL, 0, win32_bridge_out, bi_out, 0, NULL);

    /* Close child-side and original pipe handles; keep hPC alive */
    if (pty->hPipeInChild  != INVALID_HANDLE_VALUE) {
        CloseHandle(pty->hPipeInChild);
        pty->hPipeInChild = INVALID_HANDLE_VALUE;
    }
    if (pty->hPipeOutChild != INVALID_HANDLE_VALUE) {
        CloseHandle(pty->hPipeOutChild);
        pty->hPipeOutChild = INVALID_HANDLE_VALUE;
    }
    CloseHandle(pty->hPipeIn);  pty->hPipeIn  = INVALID_HANDLE_VALUE;
    CloseHandle(pty->hPipeOut); pty->hPipeOut = INVALID_HANDLE_VALUE;

    /* sv[1] is owned by bridge threads */
    win32_remove_from_map(sv[1]);

    /* PTY lifecycle: pty_process_exited fires when child exits */
    pty->child_pid  = pi.dwProcessId;
    pty->master_fd  = sv[0];
    pty->hProcess   = pi.hProcess;
    pty->hWaitHandle = NULL;
    if (!RegisterWaitForSingleObject(&pty->hWaitHandle, pty->hProcess,
            pty_process_exited, pty, INFINITE, WT_EXECUTEONLYONCE)) {
        win32_log("win32_job_run_pty: RegisterWaitForSingleObject (pty) failed: %lu\n",
            GetLastError());
        pty->hWaitHandle = NULL;
    }

    /* Register in PTY fd map so TIOCSWINSZ → win32_ioctl → win32_pty_resize works */
    win32_pty_register(sv[0], pty);

    /* Notify job_check_died via self-pipe when child exits */
    win32_job_register_exit(pi.hProcess, pi.dwProcessId);

    *out_fd = sv[0];
    win32_log("win32_job_run_pty: sv[0]=%d pid=%lu\n", sv[0], pi.dwProcessId);
    return (pid_t)pi.dwProcessId;
}
