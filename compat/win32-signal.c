/*
 * win32-signal.c - Signal emulation for tmux Windows port
 *
 * Windows does not have POSIX signals. This file provides an emulation
 * layer that uses Windows events and callbacks to simulate signal behavior.
 */

#include "win32.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Signal handler table
 */
static struct {
    sighandler_t handler;
    int pending;
} signal_handlers[NSIG];

static CRITICAL_SECTION signal_lock;
static int signal_initialized = 0;
static HANDLE signal_event = NULL;

/*
 * Console control handler for Ctrl+C, Ctrl+Break, etc.
 */
static BOOL WINAPI
console_ctrl_handler(DWORD dwCtrlType)
{
    int sig = 0;

    switch (dwCtrlType) {
    case CTRL_C_EVENT:
        sig = SIGINT;
        break;
    case CTRL_BREAK_EVENT:
        sig = SIGTERM;
        break;
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        sig = SIGHUP;
        break;
    default:
        return FALSE;
    }

    if (sig > 0 && sig < NSIG) {
        EnterCriticalSection(&signal_lock);
        signal_handlers[sig].pending = 1;
        SetEvent(signal_event);
        LeaveCriticalSection(&signal_lock);
    }

    return TRUE;
}

/*
 * Initialize the signal emulation system
 */
int
win32_signal_init(void)
{
    if (signal_initialized)
        return 0;

    InitializeCriticalSection(&signal_lock);
    memset(signal_handlers, 0, sizeof(signal_handlers));

    /* Create an event for signal notification */
    signal_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (signal_event == NULL)
        return -1;

    /* Set up console control handler */
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);

    signal_initialized = 1;
    return 0;
}

/*
 * Cleanup the signal emulation system
 */
void
win32_signal_cleanup(void)
{
    if (!signal_initialized)
        return;

    SetConsoleCtrlHandler(console_ctrl_handler, FALSE);

    if (signal_event != NULL) {
        CloseHandle(signal_event);
        signal_event = NULL;
    }

    DeleteCriticalSection(&signal_lock);
    signal_initialized = 0;
}

/*
 * Register a signal handler
 */
int
win32_signal_register(int sig, win32_signal_handler_t handler)
{
    if (sig < 0 || sig >= NSIG)
        return -1;

    win32_signal_init();

    EnterCriticalSection(&signal_lock);
    signal_handlers[sig].handler = handler;
    LeaveCriticalSection(&signal_lock);

    return 0;
}

/*
 * Process pending signals
 *
 * This should be called periodically from the event loop.
 */
void
win32_signal_process(void)
{
    int i;

    if (!signal_initialized)
        return;

    EnterCriticalSection(&signal_lock);

    for (i = 0; i < NSIG; i++) {
        if (signal_handlers[i].pending) {
            signal_handlers[i].pending = 0;

            if (signal_handlers[i].handler != NULL &&
                signal_handlers[i].handler != SIG_IGN &&
                signal_handlers[i].handler != SIG_DFL) {
                sighandler_t handler = signal_handlers[i].handler;
                LeaveCriticalSection(&signal_lock);
                handler(i);
                EnterCriticalSection(&signal_lock);
            }
        }
    }

    LeaveCriticalSection(&signal_lock);
}

/*
 * sigaction - set signal handler
 */
int
win32_sigaction_func(int sig, const struct sigaction *act, struct sigaction *oact)
{
    if (sig < 0 || sig >= NSIG) {
        errno = EINVAL;
        return -1;
    }

    win32_signal_init();

    EnterCriticalSection(&signal_lock);

    if (oact != NULL) {
        oact->sa_handler = signal_handlers[sig].handler;
        oact->sa_mask = 0;
        oact->sa_flags = 0;
    }

    if (act != NULL) {
        signal_handlers[sig].handler = act->sa_handler;
    }

    LeaveCriticalSection(&signal_lock);
    return 0;
}

/*
 * sigprocmask - block/unblock signals
 *
 * Signals are not blocked on Windows - this is a no-op.
 */
int
win32_sigprocmask(int how, const sigset_t *set, sigset_t *oldset)
{
#ifdef PLATFORM_WINDOWS
    win32_log("win32_sigprocmask: how=%d\n", how);
#endif
    static sigset_t current_mask = 0;

    (void)how;

    if (oldset != NULL)
        *oldset = current_mask;

    if (set != NULL) {
        switch (how) {
        case SIG_BLOCK:
            current_mask |= *set;
            break;
        case SIG_UNBLOCK:
            current_mask &= ~(*set);
            break;
        case SIG_SETMASK:
            current_mask = *set;
            break;
        }
    }

    return 0;
}

/*
 * Signal set manipulation functions
 */
int
win32_sigemptyset(sigset_t *set)
{
    if (set == NULL) {
        errno = EINVAL;
        return -1;
    }
    *set = 0;
    return 0;
}

int
win32_sigfillset(sigset_t *set)
{
    if (set == NULL) {
        errno = EINVAL;
        return -1;
    }
    *set = ~(sigset_t)0;
    return 0;
}

int
win32_sigaddset(sigset_t *set, int signo)
{
    if (set == NULL || signo < 0 || signo >= NSIG) {
        errno = EINVAL;
        return -1;
    }
    *set |= (1U << signo);
    return 0;
}

int
win32_sigdelset(sigset_t *set, int signo)
{
    if (set == NULL || signo < 0 || signo >= NSIG) {
        errno = EINVAL;
        return -1;
    }
    *set &= ~(1U << signo);
    return 0;
}

int
win32_sigismember(const sigset_t *set, int signo)
{
    if (set == NULL || signo < 0 || signo >= NSIG) {
        errno = EINVAL;
        return -1;
    }
    return (*set & (1U << signo)) ? 1 : 0;
}

/*
 * raise - send signal to self
 */
int
win32_raise(int sig)
{
    if (sig < 0 || sig >= NSIG) {
        errno = EINVAL;
        return -1;
    }

    win32_signal_init();

    EnterCriticalSection(&signal_lock);
    signal_handlers[sig].pending = 1;
    SetEvent(signal_event);
    LeaveCriticalSection(&signal_lock);

    return 0;
}

/*
 * Process monitoring - watch for child process exit
 *
 * This replaces SIGCHLD functionality.
 */
struct process_watch {
    HANDLE hProcess;
    HANDLE hWait;
    void (*callback)(void *);
    void *arg;
};

#define MAX_WATCHED_PROCESSES 64
static struct process_watch watched_processes[MAX_WATCHED_PROCESSES];
static CRITICAL_SECTION watch_lock;
static int watch_initialized = 0;

static void CALLBACK
process_wait_callback(PVOID lpParameter, BOOLEAN TimerOrWaitFired)
{
    struct process_watch *watch = lpParameter;

    (void)TimerOrWaitFired;

    if (watch->callback)
        watch->callback(watch->arg);
}

int
win32_process_watch(HANDLE hProcess, void (*callback)(void *), void *arg)
{
    int i;
    struct process_watch *watch = NULL;

    if (!watch_initialized) {
        InitializeCriticalSection(&watch_lock);
        memset(watched_processes, 0, sizeof(watched_processes));
        watch_initialized = 1;
    }

    EnterCriticalSection(&watch_lock);

    for (i = 0; i < MAX_WATCHED_PROCESSES; i++) {
        if (watched_processes[i].hProcess == NULL) {
            watch = &watched_processes[i];
            break;
        }
    }

    if (watch == NULL) {
        LeaveCriticalSection(&watch_lock);
        return -1;
    }

    watch->hProcess = hProcess;
    watch->callback = callback;
    watch->arg = arg;

    if (!RegisterWaitForSingleObject(
            &watch->hWait,
            hProcess,
            process_wait_callback,
            watch,
            INFINITE,
            WT_EXECUTEONLYONCE)) {
        memset(watch, 0, sizeof(*watch));
        LeaveCriticalSection(&watch_lock);
        return -1;
    }

    LeaveCriticalSection(&watch_lock);
    return 0;
}

void
win32_process_unwatch(HANDLE hProcess)
{
    int i;

    if (!watch_initialized)
        return;

    EnterCriticalSection(&watch_lock);

    for (i = 0; i < MAX_WATCHED_PROCESSES; i++) {
        if (watched_processes[i].hProcess == hProcess) {
            if (watched_processes[i].hWait != NULL) {
                UnregisterWait(watched_processes[i].hWait);
            }
            memset(&watched_processes[i], 0, sizeof(watched_processes[i]));
            break;
        }
    }

    LeaveCriticalSection(&watch_lock);
}

/*
 * strsignal implementation for Windows
 */
char *
strsignal(int sig)
{
    static char buf[32];
    
    if (sig > 0 && sig < NSIG) {
        /* In a real implementation we would map valid signals to names */
        snprintf(buf, sizeof(buf), "Signal %d", sig);
        return buf;
    }
    return "Unknown signal";
}
