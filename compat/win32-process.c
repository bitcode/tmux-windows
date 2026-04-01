/*
 * win32-process.c - Process management for tmux Windows port
 *
 * Provides POSIX process functions using Windows API.
 */

#include "win32.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Process table for tracking child processes
 */
#define MAX_CHILDREN 256

struct child_process {
    HANDLE hProcess;
    DWORD  dwProcessId;
    int    status;
    int    exited;
};

static struct child_process children[MAX_CHILDREN];
static CRITICAL_SECTION children_lock;
static int children_initialized = 0;

static void
init_children(void)
{
    if (!children_initialized) {
        InitializeCriticalSection(&children_lock);
        memset(children, 0, sizeof(children));
        children_initialized = 1;
    }
}

static int
add_child(HANDLE hProcess, DWORD dwProcessId)
{
    int i;

    init_children();
    EnterCriticalSection(&children_lock);

    for (i = 0; i < MAX_CHILDREN; i++) {
        if (children[i].hProcess == NULL) {
            children[i].hProcess = hProcess;
            children[i].dwProcessId = dwProcessId;
            children[i].status = 0;
            children[i].exited = 0;
            LeaveCriticalSection(&children_lock);
            return 0;
        }
    }

    LeaveCriticalSection(&children_lock);
    return -1;
}

static struct child_process *
find_child(pid_t pid)
{
    int i;

    for (i = 0; i < MAX_CHILDREN; i++) {
        if (children[i].dwProcessId == (DWORD)pid)
            return &children[i];
    }
    return NULL;
}

/*
 * waitpid - wait for a child process
 */
pid_t
win32_waitpid(pid_t pid, int *status, int options)
{
    struct child_process *child;
    DWORD result, exitCode;
    DWORD timeout = (options & WNOHANG) ? 0 : INFINITE;

    init_children();
    EnterCriticalSection(&children_lock);

    if (pid == -1) {
        /* Wait for any child */
        HANDLE handles[MAX_CHILDREN];
        DWORD indices[MAX_CHILDREN];
        int count = 0;
        int i;

        for (i = 0; i < MAX_CHILDREN; i++) {
            if (children[i].hProcess != NULL && !children[i].exited) {
                handles[count] = children[i].hProcess;
                indices[count] = i;
                count++;
            }
        }

        if (count == 0) {
            LeaveCriticalSection(&children_lock);
            errno = ECHILD;
            return -1;
        }

        LeaveCriticalSection(&children_lock);
        result = WaitForMultipleObjects(count, handles, FALSE, timeout);
        EnterCriticalSection(&children_lock);

        if (result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + count) {
            int idx = indices[result - WAIT_OBJECT_0];
            child = &children[idx];
        } else if (result == WAIT_TIMEOUT) {
            LeaveCriticalSection(&children_lock);
            return 0;
        } else {
            LeaveCriticalSection(&children_lock);
            errno = ECHILD;
            return -1;
        }
    } else {
        /* Wait for specific child */
        child = find_child(pid);
        if (child == NULL || child->hProcess == NULL) {
            LeaveCriticalSection(&children_lock);
            errno = ECHILD;
            return -1;
        }

        if (child->exited) {
            if (status)
                *status = child->status;
            pid_t ret = (pid_t)child->dwProcessId;
            CloseHandle(child->hProcess);
            memset(child, 0, sizeof(*child));
            LeaveCriticalSection(&children_lock);
            return ret;
        }

        LeaveCriticalSection(&children_lock);
        result = WaitForSingleObject(child->hProcess, timeout);
        EnterCriticalSection(&children_lock);

        if (result == WAIT_TIMEOUT) {
            LeaveCriticalSection(&children_lock);
            return 0;
        } else if (result != WAIT_OBJECT_0) {
            LeaveCriticalSection(&children_lock);
            errno = ECHILD;
            return -1;
        }
    }

    /* Process has exited */
    if (GetExitCodeProcess(child->hProcess, &exitCode)) {
        /* Convert to Unix-style status */
        child->status = (exitCode & 0xff) << 8;
        child->exited = 1;
    }

    if (status)
        *status = child->status;

    pid_t ret = (pid_t)child->dwProcessId;
    win32_session_log_close((long)child->dwProcessId);
    CloseHandle(child->hProcess);
    memset(child, 0, sizeof(*child));

    LeaveCriticalSection(&children_lock);
    return ret;
}

/*
 * kill - send a signal to a process
 *
 * On Windows, we can only terminate processes, not send arbitrary signals.
 */
int
win32_kill(pid_t pid, int sig)
{
    HANDLE hProcess;
    BOOL result;

    if (pid <= 0) {
        /* Process groups not supported the same way */
        errno = ESRCH;
        return -1;
    }

    if (sig == 0) {
        /* Just checking if process exists */
        hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (hProcess == NULL) {
            errno = ESRCH;
            return -1;
        }
        CloseHandle(hProcess);
        return 0;
    }

    hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (hProcess == NULL) {
        errno = ESRCH;
        return -1;
    }

    if (sig == SIGKILL || sig == SIGTERM || sig == SIGINT || sig == SIGHUP) {
        result = TerminateProcess(hProcess, 128 + sig);
        CloseHandle(hProcess);
        if (!result) {
            errno = EPERM;
            return -1;
        }
        return 0;
    }

    /* Other signals - try to send console control event */
    if (sig == SIGINT) {
        GenerateConsoleCtrlEvent(CTRL_C_EVENT, pid);
    } else if (sig == SIGBREAK) {
        GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pid);
    }

    CloseHandle(hProcess);
    return 0;
}

/*
 * killpg - send a signal to a process group
 *
 * Not directly supported on Windows.
 */
int
win32_killpg(pid_t pgrp, int sig)
{
    /* Try to kill as a single process */
    return win32_kill(pgrp, sig);
}

/*
 * User/group IDs - Windows doesn't have these in the same way
 */
uid_t win32_getuid(void)  { return 0; }
uid_t win32_geteuid(void) { return 0; }
gid_t win32_getgid(void)  { return 0; }
gid_t win32_getegid(void) { return 0; }

pid_t
win32_getpid(void)
{
    return (pid_t)GetCurrentProcessId();
}

pid_t
win32_getppid(void)
{
    /* Getting parent PID is complex on Windows, return 1 (init) */
    return 1;
}

pid_t
win32_getpgid(pid_t pid)
{
    (void)pid;
    return win32_getpid();
}

pid_t
win32_getpgrp(void)
{
    return win32_getpid();
}

int
win32_setpgid(pid_t pid, pid_t pgid)
{
    (void)pid;
    (void)pgid;
    /* Process groups not supported */
    return 0;
}

pid_t
win32_setsid(void)
{
    /* Sessions not supported in the same way */
    return win32_getpid();
}

/*
 * daemon - run as a background process
 *
 * On Windows, we use a different approach - the process must be started
 * appropriately from the beginning.
 */
int
win32_daemon(int nochdir, int noclose)
{
    (void)nochdir;
    (void)noclose;

    /*
     * On Windows, "daemonizing" would involve starting a new process
     * with CREATE_NO_WINDOW or as a service. For now, we just detach
     * from the console if possible.
     */
    FreeConsole();

    return 0;
}

/*
 * closefrom - close all file descriptors >= lowfd
 */
void
win32_closefrom(int lowfd)
{
    int i;

    /* Close C runtime file descriptors */
    for (i = lowfd; i < 2048; i++) {
        _close(i);
    }
}

/*
 * getpeereid - get peer credentials from socket
 *
 * Not supported on Windows AF_UNIX sockets.
 */
int
win32_getpeereid(int sock, uid_t *euid, gid_t *egid)
{
    (void)sock;

    /* Return root-like credentials */
    if (euid)
        *euid = 0;
    if (egid)
        *egid = 0;

    return 0;
}
