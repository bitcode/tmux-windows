/*
 * osdep-win32.c - Windows OS-dependent functions for tmux
 *
 * This file provides Windows implementations of OS-specific functions
 * that tmux uses for process information, similar to osdep-linux.c etc.
 */

#include "win32.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psapi.h>
#include <tlhelp32.h>

/* For NtQueryInformationProcess */
#include <winternl.h>

#pragma comment(lib, "psapi.lib")

typedef NTSTATUS (WINAPI *NtQIPfn)(HANDLE, int, PVOID, ULONG, PULONG);
static NtQIPfn NtQIP = NULL;

static NtQIPfn
get_ntqip(void)
{
    if (NtQIP == NULL) {
        HMODULE ntdll = GetModuleHandleA("ntdll.dll");
        if (ntdll != NULL)
            NtQIP = (NtQIPfn)GetProcAddress(ntdll, "NtQueryInformationProcess");
    }
    return NtQIP;
}

/*
 * Get the name of the process running in a pane.
 *
 * On Windows, we use process handle to query the executable name.
 */
char *
osdep_get_name(int fd, char *tty)
{
    (void)fd;  /* Not used on Windows */
    (void)tty;

    /* For now, return a default - proper implementation needs
     * the pane's process handle */
    return NULL;
}

/*
 * Get the current working directory of a process via NtQueryInformationProcess.
 * Reads RTL_USER_PROCESS_PARAMETERS.CurrentDirectory from the target PEB.
 */
char *
osdep_get_cwd(int fd)
{
    static char cwd_buf[MAX_PATH];
    DWORD child_pid;
    HANDLE hProcess;
    PROCESS_BASIC_INFORMATION pbi;
    ULONG ret_len;
    PVOID peb_base, params_addr;
    PVOID cur_dir_buf_ptr;
    USHORT cur_dir_len;
    SIZE_T bytes_read;
    wchar_t *wbuf;
    size_t wlen;
    char *result = NULL;

    child_pid = win32_pty_get_child_pid(fd);
    if (child_pid == 0)
        return NULL;

    if (get_ntqip() == NULL)
        return NULL;

    hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                           FALSE, child_pid);
    if (hProcess == NULL)
        return NULL;

    __try {
        if (NtQIP(hProcess, 0 /* ProcessBasicInformation */,
                  &pbi, sizeof(pbi), &ret_len) != 0)
            goto fail;

        peb_base = pbi.PebBaseAddress;

        /* Read PEB->ProcessParameters pointer */
#ifdef _WIN64
        if (!ReadProcessMemory(hProcess, (BYTE*)peb_base + 0x20,
                               &params_addr, sizeof(params_addr), &bytes_read))
            goto fail;
        /* CurrentDirectory.Length at params+0x38, Buffer pointer at params+0x40 */
        if (!ReadProcessMemory(hProcess, (BYTE*)params_addr + 0x38,
                               &cur_dir_len, sizeof(cur_dir_len), &bytes_read))
            goto fail;
        if (!ReadProcessMemory(hProcess, (BYTE*)params_addr + 0x40,
                               &cur_dir_buf_ptr, sizeof(cur_dir_buf_ptr), &bytes_read))
            goto fail;
#else
        if (!ReadProcessMemory(hProcess, (BYTE*)peb_base + 0x10,
                               &params_addr, sizeof(params_addr), &bytes_read))
            goto fail;
        /* offset 0x24 on x86 */
        if (!ReadProcessMemory(hProcess, (BYTE*)params_addr + 0x24,
                               &cur_dir_len, sizeof(cur_dir_len), &bytes_read))
            goto fail;
        if (!ReadProcessMemory(hProcess, (BYTE*)params_addr + 0x28,
                               &cur_dir_buf_ptr, sizeof(cur_dir_buf_ptr), &bytes_read))
            goto fail;
#endif

        if (cur_dir_len == 0 || cur_dir_len > 32767 || cur_dir_buf_ptr == NULL)
            goto fail;

        wbuf = malloc(cur_dir_len + sizeof(wchar_t));
        if (wbuf == NULL)
            goto fail;
        if (!ReadProcessMemory(hProcess, cur_dir_buf_ptr, wbuf,
                               cur_dir_len, &bytes_read)) {
            free(wbuf);
            goto fail;
        }
        wbuf[cur_dir_len / sizeof(wchar_t)] = L'\0';

        /* Strip trailing backslash unless root (e.g. C:\) */
        wlen = wcslen(wbuf);
        if (wlen > 3 && wbuf[wlen - 1] == L'\\')
            wbuf[wlen - 1] = L'\0';

        WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, cwd_buf, sizeof(cwd_buf), NULL, NULL);
        free(wbuf);
        CloseHandle(hProcess);
        return cwd_buf;

fail:
        CloseHandle(hProcess);
        return NULL;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        /* Access violation or other fault reading remote process memory */
        CloseHandle(hProcess);
        return NULL;
    }
    (void)result;
    return NULL;
}

/*
 * Get the command line of a pane's foreground process.
 *
 * Uses NtQueryInformationProcess class 60 (ProcessCommandLineInformation),
 * available on Windows 8.1+. The OS copies the UNICODE_STRING header plus
 * string content into the caller's buffer — no ReadProcessMemory needed.
 */
char *
osdep_get_cmdline(int fd, char *tty)
{
    static char cmdline_buf[32768];
    DWORD child_pid;
    HANDLE hProcess;
    ULONG ret_len;
    BYTE *buf = NULL;
    ULONG bufsz;
    NTSTATUS st;
    UNICODE_STRING *us;
    (void)tty;

    child_pid = win32_pty_get_child_pid(fd);
    if (child_pid == 0)
        return NULL;

    if (get_ntqip() == NULL)
        return NULL;

    hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, child_pid);
    if (hProcess == NULL)
        return NULL;

    __try {
        /* First call: get required buffer size. */
        ret_len = 0;
        st = NtQIP(hProcess, 60 /* ProcessCommandLineInformation */,
                   NULL, 0, &ret_len);
        /* STATUS_INFO_LENGTH_MISMATCH = 0xC0000004; any non-zero ret_len means we got the size. */
        if (ret_len == 0)
            goto fail;

        bufsz = ret_len + sizeof(UNICODE_STRING);
        buf = malloc(bufsz);
        if (buf == NULL)
            goto fail;

        ret_len = 0;
        st = NtQIP(hProcess, 60, buf, bufsz, &ret_len);
        if (st != 0)
            goto fail;

        us = (UNICODE_STRING *)buf;
        if (us->Length == 0 || us->Buffer == NULL)
            goto fail;

        WideCharToMultiByte(CP_UTF8, 0, us->Buffer, us->Length / sizeof(wchar_t),
                            cmdline_buf, sizeof(cmdline_buf) - 1, NULL, NULL);
        cmdline_buf[sizeof(cmdline_buf) - 1] = '\0';
        free(buf);
        CloseHandle(hProcess);
        return cmdline_buf;

fail:
        free(buf);
        CloseHandle(hProcess);
        return NULL;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        free(buf);
        CloseHandle(hProcess);
        return NULL;
    }
}

/*
 * Get process name from process ID
 */
char *
win32_get_process_name(DWORD pid)
{
    static char name[MAX_PATH];
    HANDLE hProcess;
    HMODULE hMod;
    DWORD cbNeeded;

    hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                          FALSE, pid);
    if (hProcess == NULL)
        return NULL;

    if (EnumProcessModules(hProcess, &hMod, sizeof(hMod), &cbNeeded)) {
        if (GetModuleBaseNameA(hProcess, hMod, name, sizeof(name))) {
            CloseHandle(hProcess);
            return name;
        }
    }

    CloseHandle(hProcess);
    return NULL;
}

/*
 * Get process executable path from process ID
 */
char *
win32_get_process_path(DWORD pid)
{
    static char path[MAX_PATH];
    HANDLE hProcess;
    DWORD size = sizeof(path);

    hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProcess == NULL)
        return NULL;

    if (QueryFullProcessImageNameA(hProcess, 0, path, &size)) {
        CloseHandle(hProcess);
        return path;
    }

    CloseHandle(hProcess);
    return NULL;
}

/*
 * Get process current working directory
 *
 * This is quite complex on Windows and requires reading from the
 * Process Environment Block (PEB) of the target process.
 */
char *
win32_get_process_cwd(DWORD pid)
{
    /* This would require:
     * 1. Opening the process with PROCESS_QUERY_INFORMATION | PROCESS_VM_READ
     * 2. Using NtQueryInformationProcess to get the PEB address
     * 3. Reading the RTL_USER_PROCESS_PARAMETERS from the PEB
     * 4. Reading the CurrentDirectory from the parameters
     *
     * For now, return NULL as this is complex and may fail on protected processes.
     */
    (void)pid;
    return NULL;
}

/*
 * Check if a process is running
 */
int
win32_process_is_running(DWORD pid)
{
    HANDLE hProcess;
    DWORD exitCode;

    hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProcess == NULL)
        return 0;

    if (!GetExitCodeProcess(hProcess, &exitCode)) {
        CloseHandle(hProcess);
        return 0;
    }

    CloseHandle(hProcess);
    return exitCode == STILL_ACTIVE;
}

/*
 * Get parent process ID
 */
DWORD
win32_get_parent_pid(DWORD pid)
{
    HANDLE hSnapshot;
    PROCESSENTRY32 pe32;
    DWORD ppid = 0;

    hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE)
        return 0;

    pe32.dwSize = sizeof(PROCESSENTRY32);

    if (Process32First(hSnapshot, &pe32)) {
        do {
            if (pe32.th32ProcessID == pid) {
                ppid = pe32.th32ParentProcessID;
                break;
            }
        } while (Process32Next(hSnapshot, &pe32));
    }

    CloseHandle(hSnapshot);
    return ppid;
}

/*
 * Get list of child processes
 */
int
win32_get_child_pids(DWORD ppid, DWORD *pids, int max_pids)
{
    HANDLE hSnapshot;
    PROCESSENTRY32 pe32;
    int count = 0;

    hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE)
        return 0;

    pe32.dwSize = sizeof(PROCESSENTRY32);

    if (Process32First(hSnapshot, &pe32)) {
        do {
            if (pe32.th32ParentProcessID == ppid && count < max_pids) {
                pids[count++] = pe32.th32ProcessID;
            }
        } while (Process32Next(hSnapshot, &pe32));
    }

    CloseHandle(hSnapshot);
    return count;
}

/*
 * Get the full executable path of the parent process.
 * Used to auto-detect the launching shell (e.g., pwsh.exe, cmd.exe).
 * Returns a static buffer, or NULL on failure.
 */
const char *
win32_get_parent_shell(void)
{
    static char path[MAX_PATH];
    DWORD ppid, len;
    HANDLE hp;

    ppid = win32_get_parent_pid(GetCurrentProcessId());
    if (ppid == 0)
        return NULL;

    hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, ppid);
    if (hp == NULL)
        return NULL;

    len = MAX_PATH;
    if (!QueryFullProcessImageNameA(hp, 0, path, &len)) {
        CloseHandle(hp);
        return NULL;
    }
    CloseHandle(hp);
    return path;
}

/*
 * Get username
 */
char *
win32_get_username(void)
{
    static char username[256];
    DWORD size = sizeof(username);

    if (GetUserNameA(username, &size))
        return username;

    return "unknown";
}

/*
 * Get hostname
 */
char *
win32_get_hostname(void)
{
    static char hostname[256];
    DWORD size = sizeof(hostname);

    if (GetComputerNameA(hostname, &size))
        return hostname;

    return "localhost";
}

/*
 * Check if running as administrator
 */
int
win32_is_admin(void)
{
    BOOL isAdmin = FALSE;
    PSID adminGroup = NULL;
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;

    if (AllocateAndInitializeSid(&ntAuthority, 2,
            SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
            0, 0, 0, 0, 0, 0, &adminGroup)) {
        CheckTokenMembership(NULL, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }

    return isAdmin;
}

/*
 * Get Windows version info
 */
void
win32_get_version(int *major, int *minor, int *build)
{
    OSVERSIONINFOEXW osvi;
    typedef NTSTATUS (WINAPI *RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
    RtlGetVersionPtr RtlGetVersion;
    HMODULE ntdll;

    memset(&osvi, 0, sizeof(osvi));
    osvi.dwOSVersionInfoSize = sizeof(osvi);

    ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll) {
        RtlGetVersion = (RtlGetVersionPtr)GetProcAddress(ntdll, "RtlGetVersion");
        if (RtlGetVersion) {
            RtlGetVersion((PRTL_OSVERSIONINFOW)&osvi);
        }
    }

    if (major) *major = osvi.dwMajorVersion;
    if (minor) *minor = osvi.dwMinorVersion;
    if (build) *build = osvi.dwBuildNumber;
}

/*
 * Check if ConPTY is available
 */
int
win32_has_conpty(void)
{
    int major, minor, build;

    win32_get_version(&major, &minor, &build);

    /* ConPTY available in Windows 10 1809 (build 17763) and later */
    if (major > 10)
        return 1;
    if (major == 10 && build >= 17763)
        return 1;

    return 0;
}

/*
 * Check if AF_UNIX is available
 */
int
win32_has_afunix(void)
{
    int major, minor, build;

    win32_get_version(&major, &minor, &build);

    /* AF_UNIX available in Windows 10 1803 (build 17134) and later */
    if (major > 10)
        return 1;
    if (major == 10 && build >= 17134)
        return 1;

    return 0;
}

#include <fcntl.h>
#include <io.h>

int
getptmfd(void)
{
    /* return a dummy fd */
    return open("NUL", O_RDWR);
}

struct event_base *
osdep_event_init(void)
{
    return event_init();
}
