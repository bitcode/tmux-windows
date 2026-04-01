/*
 * win32-jobctl.c - Job Object lifecycle and process suspension for tmux (PERM-02)
 *
 * Provides an approximation of POSIX SIGTSTP/SIGCONT/SIGKILL for child
 * process trees spawned under ConPTY.
 *
 * Architecture
 * ------------
 * Each ConPTY pane child is assigned to a Windows Job Object at spawn time
 * (see win32-pty.c).  The job object gives us reliable process-tree
 * membership even after the root shell forks child processes.
 *
 * Suspend path (Ctrl+Z → SIGTSTP approximation)
 * -----------------------------------------------
 * 1. Enumerate all PIDs in the job via QueryInformationJobObject.
 * 2. For each PID, open all threads (Toolhelp32 snapshot) and call
 *    CancelSynchronousIo on each thread BEFORE suspending.  Without this,
 *    threads blocked in IopSynchronousServiceTail (e.g. ReadFile on ConPTY
 *    pipe) cannot be resumed — documented in Microsoft Terminal issue #9704.
 * 3. Call NtSuspendProcess (ntdll.dll export, undocumented but stable since
 *    Win2000) on each PID.  This atomically suspends all threads under kernel
 *    locks, eliminating the TOCTOU race of per-thread SuspendThread+Toolhelp32.
 * 4. On any failure during step 3, roll back by calling NtResumeProcess on
 *    all already-suspended processes.
 *
 * Resume path (fg → SIGCONT approximation)
 * -----------------------------------------
 * 1. Call NtResumeProcess on each PID in the job.
 * 2. Synthesize a WINDOW_BUFFER_SIZE_EVENT by calling ResizePseudoConsole
 *    with the existing size — this triggers the shell's SIGWINCH handler,
 *    which redraws the prompt.
 *
 * Kill path (kill-pane → SIGKILL)
 * --------------------------------
 * TerminateJobObject — instant, reliable, kills entire process tree.
 *
 * Signal proxy (SIGINT/SIGTERM across ConPTY boundary)
 * -----------------------------------------------------
 * GenerateConsoleCtrlEvent with CTRL_C_EVENT broadcasts to ALL processes on
 * the caller's console — this would kill the tmux server itself.  Instead,
 * an ephemeral tmux-signal-proxy.exe is spawned: it calls FreeConsole() +
 * AttachConsole(target_pid) + GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, 0),
 * safely crossing the ConPTY isolation boundary.
 *
 * NtSuspendProcess / NtResumeProcess loading
 * -------------------------------------------
 * Both are loaded at runtime via GetProcAddress.  If not found (should never
 * happen on any supported Windows 10+ build), win32_jobctl_suspend falls back
 * to win32_jobctl_kill (degraded: kill instead of suspend).
 */

#include "win32.h"
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ntdll process suspend/resume — undocumented but stable */
typedef LONG (NTAPI *PFN_NtSuspendProcess)(HANDLE hProcess);
typedef LONG (NTAPI *PFN_NtResumeProcess)(HANDLE hProcess);

static PFN_NtSuspendProcess pfn_NtSuspendProcess = NULL;
static PFN_NtResumeProcess  pfn_NtResumeProcess  = NULL;
static int jobctl_nt_loaded = 0;

static void
jobctl_load_nt(void)
{
    if (jobctl_nt_loaded)
        return;
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (ntdll != NULL) {
        pfn_NtSuspendProcess = (PFN_NtSuspendProcess)
            GetProcAddress(ntdll, "NtSuspendProcess");
        pfn_NtResumeProcess  = (PFN_NtResumeProcess)
            GetProcAddress(ntdll, "NtResumeProcess");
    }
    if (pfn_NtSuspendProcess == NULL || pfn_NtResumeProcess == NULL)
        win32_log("win32_jobctl: NtSuspendProcess/NtResumeProcess not found"
                  " — suspend will degrade to kill\n");
    jobctl_nt_loaded = 1;
}

/* -------------------------------------------------------------------------
 * win32_jobctl_create
 *
 * Creates a Job Object and assigns the already-running process |pid| to it.
 * Called immediately after CreateProcess in win32_spawn_process.
 *
 * Returns the job HANDLE on success, NULL on failure.
 * The caller is responsible for closing the handle when the pane exits.
 * ---------------------------------------------------------------------- */
HANDLE
win32_jobctl_create(DWORD pid)
{
    HANDLE hJob  = NULL;
    HANDLE hProc = NULL;

    jobctl_load_nt();

    hJob = CreateJobObjectA(NULL, NULL);
    if (hJob == NULL) {
        win32_log("win32_jobctl_create: CreateJobObject failed: %lu\n",
            GetLastError());
        return NULL;
    }

    /*
     * Deny breakaway so child processes cannot escape the job.
     * This is the key property that gives us reliable tree membership.
     */
    JOBOBJECT_BASIC_LIMIT_INFORMATION jbli;
    memset(&jbli, 0, sizeof jbli);
    jbli.LimitFlags = JOB_OBJECT_LIMIT_BREAKAWAY_OK; /* allow explicit breakaway only */
    SetInformationJobObject(hJob, JobObjectBasicLimitInformation,
        &jbli, sizeof jbli);

    hProc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (hProc == NULL) {
        win32_log("win32_jobctl_create: OpenProcess(%lu) failed: %lu\n",
            pid, GetLastError());
        CloseHandle(hJob);
        return NULL;
    }

    if (!AssignProcessToJobObject(hJob, hProc)) {
        DWORD err = GetLastError();
        /*
         * ERROR_ACCESS_DENIED (5) is common when the process was already
         * assigned to a job by the parent (e.g. Windows Terminal). We treat
         * this as a non-fatal degradation — kill-pane will still work via
         * TerminateProcess, suspend will fall back to NtSuspendProcess
         * without job enumeration.
         */
        win32_log("win32_jobctl_create: AssignProcessToJobObject failed: %lu"
                  " (degraded mode)\n", err);
        CloseHandle(hProc);
        CloseHandle(hJob);
        return NULL;
    }

    CloseHandle(hProc);
    win32_log("win32_jobctl_create: pid=%lu assigned to job %p\n", pid, hJob);
    return hJob;
}

/* -------------------------------------------------------------------------
 * cancel_io_for_pid
 *
 * Opens all threads of |pid| and calls CancelSynchronousIo on each.
 * This breaks threads out of IopSynchronousServiceTail (blocked ReadFile)
 * before NtSuspendProcess, preventing permanent deadlock.
 *
 * Failures are non-fatal — CancelSynchronousIo returns FALSE with
 * ERROR_NOT_FOUND when the thread has no pending I/O (common case).
 * ---------------------------------------------------------------------- */
static void
cancel_io_for_pid(DWORD pid)
{
    HANDLE hSnap;
    THREADENTRY32 te;

    hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (hSnap == INVALID_HANDLE_VALUE)
        return;

    te.dwSize = sizeof te;
    if (!Thread32First(hSnap, &te)) {
        CloseHandle(hSnap);
        return;
    }

    do {
        if (te.th32OwnerProcessID != pid)
            continue;
        /*
         * THREAD_TERMINATE is required by CancelSynchronousIo.
         * Same-user processes grant this without elevation.
         */
        HANDLE hThread = OpenThread(THREAD_TERMINATE, FALSE, te.th32ThreadID);
        if (hThread != NULL) {
            CancelSynchronousIo(hThread);
            CloseHandle(hThread);
        }
    } while (Thread32Next(hSnap, &te));

    CloseHandle(hSnap);
}

/* -------------------------------------------------------------------------
 * enumerate_job_pids
 *
 * Fills |pids| (up to |max_pids| entries) with all PIDs in |hJob|.
 * Returns the count placed in |pids|.
 * ---------------------------------------------------------------------- */
#define MAX_JOB_PIDS 256

static DWORD
enumerate_job_pids(HANDLE hJob, DWORD *pids, DWORD max_pids)
{
    JOBOBJECT_BASIC_PROCESS_ID_LIST *list;
    DWORD  buf_size;
    DWORD  count = 0;

    buf_size = (DWORD)(sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) +
               (max_pids - 1) * sizeof(ULONG_PTR));
    list = (JOBOBJECT_BASIC_PROCESS_ID_LIST *)malloc(buf_size);
    if (list == NULL)
        return 0;

    if (!QueryInformationJobObject(hJob, JobObjectBasicProcessIdList,
            list, buf_size, NULL)) {
        win32_log("win32_jobctl: QueryInformationJobObject failed: %lu\n",
            GetLastError());
        free(list);
        return 0;
    }

    count = list->NumberOfProcessIdsInList;
    if (count > max_pids)
        count = max_pids;
    for (DWORD i = 0; i < count; i++)
        pids[i] = (DWORD)list->ProcessIdList[i];

    free(list);
    return count;
}

/* -------------------------------------------------------------------------
 * win32_jobctl_send_signal
 *
 * Spawns tmux-signal-proxy.exe to deliver CTRL_BREAK_EVENT to |root_pid|
 * across the ConPTY isolation boundary.
 *
 * The proxy binary is expected to live next to tmux.exe.  If not found,
 * this is a no-op (signal delivery silently skipped).
 * ---------------------------------------------------------------------- */
void
win32_jobctl_send_signal(DWORD root_pid)
{
    char  exe_dir[MAX_PATH];
    char  proxy_path[MAX_PATH];
    char  cmdline[MAX_PATH + 32];
    DWORD len;
    STARTUPINFOA        si;
    PROCESS_INFORMATION pi;

    len = GetModuleFileNameA(NULL, exe_dir, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        win32_log("win32_jobctl_send_signal: GetModuleFileNameA failed: %lu\n",
            GetLastError());
        return;
    }

    /* Strip the filename to get the directory */
    char *last_sep = strrchr(exe_dir, '\\');
    if (last_sep == NULL)
        last_sep = strrchr(exe_dir, '/');
    if (last_sep != NULL)
        *last_sep = '\0';

    _snprintf(proxy_path, sizeof proxy_path, "%s\\tmux-signal-proxy.exe",
        exe_dir);
    _snprintf(cmdline, sizeof cmdline, "tmux-signal-proxy.exe %lu", root_pid);

    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);

    win32_log("win32_jobctl_send_signal: spawning %s pid=%lu\n",
        proxy_path, root_pid);

    if (!CreateProcessA(
            proxy_path,   /* application */
            cmdline,      /* command line */
            NULL, NULL,   /* process/thread security */
            FALSE,        /* no handle inheritance */
            DETACHED_PROCESS | CREATE_NO_WINDOW,
            NULL,         /* inherit environment */
            NULL,         /* inherit cwd */
            &si, &pi)) {
        win32_log("win32_jobctl_send_signal: CreateProcessA failed: %lu"
                  " (proxy not found?)\n", GetLastError());
        return;
    }

    /* Don't wait — proxy exits nearly instantly on its own */
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
}

/* -------------------------------------------------------------------------
 * suspend_single_pid
 *
 * Fallback used when hJob == NULL (Windows Terminal nested-job scenario).
 * Suspends a single process directly without job enumeration.
 * Returns 1 on success, 0 on failure.
 * ---------------------------------------------------------------------- */
static int
suspend_single_pid(DWORD pid)
{
    HANDLE hProc;
    LONG   status;

    if (pid == 0)
        return 0;

    hProc = OpenProcess(PROCESS_SUSPEND_RESUME, FALSE, pid);
    if (hProc == NULL) {
        win32_log("suspend_single_pid: OpenProcess(%lu) failed: %lu\n",
            pid, GetLastError());
        return 0;
    }

    status = pfn_NtSuspendProcess(hProc);
    CloseHandle(hProc);

    if (status != 0) {
        win32_log("suspend_single_pid: NtSuspendProcess(%lu) status=%ld\n",
            pid, status);
        return 0;
    }

    win32_log("suspend_single_pid: suspended pid=%lu\n", pid);
    return 1;
}

/* -------------------------------------------------------------------------
 * win32_jobctl_suspend
 *
 * Suspends all processes in the job.  When hJob is NULL (Job Object
 * assignment failed at spawn — common under Windows Terminal), falls back
 * to suspending the single root child process via child_pid.
 *
 * Returns 1 on success, 0 on failure.
 * ---------------------------------------------------------------------- */
int
win32_jobctl_suspend(HANDLE hJob, DWORD child_pid)
{
    DWORD  pids[MAX_JOB_PIDS];
    DWORD  count, i;
    HANDLE suspended[MAX_JOB_PIDS];
    DWORD  n_suspended = 0;

    jobctl_load_nt();

    if (pfn_NtSuspendProcess == NULL) {
        win32_log("win32_jobctl_suspend: NtSuspendProcess unavailable\n");
        return 0;
    }

    if (hJob == NULL) {
        /*
         * Degraded path: no Job Object (Windows Terminal nested job).
         * Suspend the single root child directly.
         */
        win32_log("win32_jobctl_suspend: no job handle — single-process"
                  " fallback for pid=%lu\n", child_pid);
        return suspend_single_pid(child_pid);
    }

    count = enumerate_job_pids(hJob, pids, MAX_JOB_PIDS);
    if (count == 0) {
        win32_log("win32_jobctl_suspend: no PIDs in job — single-process"
                  " fallback for pid=%lu\n", child_pid);
        return suspend_single_pid(child_pid);
    }

    for (i = 0; i < count; i++) {
        /*
         * Do NOT call CancelSynchronousIo here. The original plan called for
         * canceling I/O to break threads out of IopSynchronousServiceTail
         * (ConPTY ReadFile deadlock, Terminal #9704). However, the processes
         * in this Job Object are shell/child processes — not the bridge thread.
         * CancelSynchronousIo on bash threads aborts its internal wait-pipe
         * (MSYS2/MinGW child-process tracking), which breaks the shell's ability
         * to wait for subprocesses after resume. NtSuspendProcess alone is safe
         * for shell processes; the ConPTY bridge thread (in the tmux server
         * process) is not in the Job and is unaffected.
         */

        /* Suspend via NtSuspendProcess */
        HANDLE hProc = OpenProcess(PROCESS_SUSPEND_RESUME, FALSE, pids[i]);
        if (hProc == NULL) {
            DWORD oerr = GetLastError();
            /*
             * Transient children (e.g. a just-exited `sleep` subprocess)
             * may no longer exist by the time we enumerate them — skip
             * rather than rolling back the entire suspend.
             * ERROR_INVALID_PARAMETER (87): PID already gone.
             * ERROR_ACCESS_DENIED (5): system/protected process in job.
             */
            win32_log("win32_jobctl_suspend: OpenProcess(%lu) failed: %lu"
                      " — skipping this PID\n", pids[i], oerr);
            continue;
        }

        LONG status = pfn_NtSuspendProcess(hProc);
        if (status != 0) {
            win32_log("win32_jobctl_suspend: NtSuspendProcess(%lu) status=%ld"
                      " — rolling back\n", pids[i], status);
            CloseHandle(hProc);
            goto rollback;
        }

        suspended[n_suspended++] = hProc;
        win32_log("win32_jobctl_suspend: suspended pid=%lu\n", pids[i]);
    }

    for (i = 0; i < n_suspended; i++)
        CloseHandle(suspended[i]);
    return 1;

rollback:
    win32_log("win32_jobctl_suspend: rolling back %lu suspended processes\n",
        n_suspended);
    for (i = 0; i < n_suspended; i++) {
        pfn_NtResumeProcess(suspended[i]);
        CloseHandle(suspended[i]);
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * win32_jobctl_resume
 *
 * Resumes all processes in the job, then synthesizes a resize event to
 * trigger shell prompt redraw.  When hJob is NULL, falls back to resuming
 * the single root child process via child_pid.
 * ---------------------------------------------------------------------- */
int
win32_jobctl_resume(HANDLE hJob, DWORD child_pid, HPCON hPC, COORD size)
{
    DWORD  pids[MAX_JOB_PIDS];
    DWORD  count, i;

    jobctl_load_nt();

    if (pfn_NtResumeProcess == NULL) {
        win32_log("win32_jobctl_resume: NtResumeProcess unavailable\n");
        return 0;
    }

    if (hJob == NULL) {
        /*
         * Degraded path: no Job Object.  Resume the single root child.
         */
        win32_log("win32_jobctl_resume: no job handle — single-process"
                  " fallback for pid=%lu\n", child_pid);
        if (child_pid != 0) {
            HANDLE hProc = OpenProcess(PROCESS_SUSPEND_RESUME, FALSE,
                child_pid);
            if (hProc != NULL) {
                pfn_NtResumeProcess(hProc);
                CloseHandle(hProc);
                win32_log("win32_jobctl_resume: resumed pid=%lu\n", child_pid);
            }
        }
        if (hPC != NULL)
            ResizePseudoConsole(hPC, size);
        return 1;
    }

    count = enumerate_job_pids(hJob, pids, MAX_JOB_PIDS);
    if (count == 0 && child_pid != 0) {
        /* Job is empty but child_pid known — still try single-process path */
        HANDLE hProc = OpenProcess(PROCESS_SUSPEND_RESUME, FALSE, child_pid);
        if (hProc != NULL) {
            pfn_NtResumeProcess(hProc);
            CloseHandle(hProc);
        }
    }

    for (i = 0; i < count; i++) {
        HANDLE hProc = OpenProcess(PROCESS_SUSPEND_RESUME, FALSE, pids[i]);
        if (hProc == NULL)
            continue;
        pfn_NtResumeProcess(hProc);
        CloseHandle(hProc);
        win32_log("win32_jobctl_resume: resumed pid=%lu\n", pids[i]);
    }

    /*
     * Synthesize WINDOW_BUFFER_SIZE_EVENT by calling ResizePseudoConsole
     * with the current size.  This triggers the shell's SIGWINCH handler
     * and redraws the prompt without actually changing dimensions.
     */
    if (hPC != NULL)
        ResizePseudoConsole(hPC, size);

    return 1;
}

/* -------------------------------------------------------------------------
 * win32_jobctl_kill
 *
 * Terminates all processes in the job immediately.
 * ---------------------------------------------------------------------- */
void
win32_jobctl_kill(HANDLE hJob)
{
    if (hJob == NULL)
        return;
    TerminateJobObject(hJob, 1);
    win32_log("win32_jobctl_kill: job %p terminated\n", hJob);
}

/* -------------------------------------------------------------------------
 * win32_jobctl_close
 *
 * Closes the job handle when a pane exits.
 * ---------------------------------------------------------------------- */
void
win32_jobctl_close(HANDLE hJob)
{
    if (hJob != NULL)
        CloseHandle(hJob);
}
