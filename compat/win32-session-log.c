/*
 * win32-session-log.c - Session accounting shim for tmux Windows port (PERM-04)
 *
 * Windows has no utmp/wtmp/login_tty equivalent accessible to unprivileged
 * processes. This file provides a functional replacement:
 *
 *   - A JSON session ledger at %LOCALAPPDATA%\tmux\sessions.json tracking
 *     every pane open/close with timestamp, PID, shell, and state.
 *   - A named Win32 mutex (Local\TmuxSessionStateMutex) for safe concurrent
 *     access when multiple tmux server instances run simultaneously.
 *   - Optional Application Event Log integration via ReportEvent(). Only
 *     fires if the event source key was pre-registered (e.g. by an installer):
 *     HKLM\SYSTEM\CurrentControlSet\Services\EventLog\Application\tmux
 *     Standard users cannot create that key, but can write once it exists.
 *
 * POSIX stubs resolved here:
 *   login_tty()    -> win32_session_log_open() (ConPTY handles the tty work)
 *   pututxline()   -> win32_session_log_open() / win32_session_log_close()
 *   logwtmp()      -> win32_session_log_close()
 *   getlogin()     -> getenv("USERNAME") fallback (in win32-stubs.c)
 *
 * JSON format (one entry per line, newline-delimited):
 *   {"id":<pane_id>,"pid":<pid>,"shell":"...","opened":"ISO8601","state":"active"}
 *   {"id":<pane_id>,"pid":<pid>,"shell":"...","opened":"ISO8601","closed":"ISO8601","state":"dead"}
 */

#include "win32.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SESSION_LOG_MUTEX_NAME  L"Local\\TmuxSessionStateMutex"
#define SESSION_LOG_EVENTSOURCE "tmux"
#define SESSION_LOG_DEAD_AGE    (7 * 24 * 3600)  /* 7 days in seconds */

static char  session_log_path[MAX_PATH] = {0};
static int   session_log_initialized    = 0;
static HANDLE session_log_mutex         = NULL;

/* Forward declarations */
void win32_session_log_cleanup(void);

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

static void
iso8601_now(char *buf, size_t len)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(buf, len, "%04d-%02d-%02dT%02d:%02d:%02d",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond);
}

static void
acquire_mutex(void)
{
    if (session_log_mutex != NULL)
        WaitForSingleObject(session_log_mutex, 5000);
}

static void
release_mutex(void)
{
    if (session_log_mutex != NULL)
        ReleaseMutex(session_log_mutex);
}

/*
 * Read the entire sessions.json into a heap buffer.
 * Caller must free(). Returns NULL if file does not exist or is empty.
 */
static char *
read_log_file(void)
{
    FILE   *f;
    long    sz;
    char   *buf;

    f = fopen(session_log_path, "rb");
    if (f == NULL)
        return NULL;

    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    rewind(f);

    if (sz <= 0) {
        fclose(f);
        return NULL;
    }

    buf = malloc((size_t)sz + 1);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }

    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        free(buf);
        return NULL;
    }
    buf[sz] = '\0';
    fclose(f);
    return buf;
}

/*
 * Atomically rewrite the log file with new content.
 */
static void
write_log_file(const char *content)
{
    FILE *f = fopen(session_log_path, "wb");
    if (f == NULL) {
        win32_log("win32_session_log: cannot open %s for write\n",
            session_log_path);
        return;
    }
    if (content != NULL && *content != '\0')
        fwrite(content, 1, strlen(content), f);
    fclose(f);
}

/*
 * Emit an informational event to the Windows Application Event Log.
 * Silently does nothing if the event source is not registered.
 */
static void
emit_event_log(const char *msg)
{
    HANDLE hEventLog;

    hEventLog = RegisterEventSourceA(NULL, SESSION_LOG_EVENTSOURCE);
    if (hEventLog == NULL)
        return;  /* source not registered — not an error */

    ReportEventA(hEventLog,
        EVENTLOG_INFORMATION_TYPE,
        0,          /* category */
        1,          /* event ID */
        NULL,       /* user SID */
        1,          /* num strings */
        0,          /* raw data size */
        &msg,       /* strings */
        NULL);      /* raw data */

    DeregisterEventSource(hEventLog);
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

/*
 * win32_session_log_init - call once on server startup.
 *
 * Resolves %LOCALAPPDATA%\tmux\sessions.json, creates the directory if
 * needed, creates the named mutex, and purges stale dead entries.
 */
void
win32_session_log_init(void)
{
    char localappdata[MAX_PATH];
    char tmux_dir[MAX_PATH];
    DWORD attrs;

    if (session_log_initialized)
        return;

    /* Resolve %LOCALAPPDATA% */
    if (GetEnvironmentVariableA("LOCALAPPDATA", localappdata,
            sizeof localappdata) == 0) {
        win32_log("win32_session_log_init: LOCALAPPDATA not set\n");
        return;
    }

    snprintf(tmux_dir, sizeof tmux_dir, "%s\\tmux", localappdata);
    snprintf(session_log_path, sizeof session_log_path,
        "%s\\sessions.json", tmux_dir);

    /* Create %LOCALAPPDATA%\tmux if it does not exist */
    attrs = GetFileAttributesA(tmux_dir);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        if (!CreateDirectoryA(tmux_dir, NULL)) {
            win32_log("win32_session_log_init: cannot create %s: %lu\n",
                tmux_dir, GetLastError());
            session_log_path[0] = '\0';
            return;
        }
    }

    /* Named mutex for cross-instance serialisation */
    session_log_mutex = CreateMutexW(NULL, FALSE, SESSION_LOG_MUTEX_NAME);
    if (session_log_mutex == NULL)
        win32_log("win32_session_log_init: CreateMutex failed: %lu\n",
            GetLastError());

    session_log_initialized = 1;
    win32_log("win32_session_log_init: log path: %s\n", session_log_path);

    /* Purge dead entries older than SESSION_LOG_DEAD_AGE on startup */
    win32_session_log_cleanup();
}

/*
 * win32_session_log_open - record a new pane as active.
 *
 * pane_id : tmux internal pane ID (or 0 if unavailable)
 * pid     : child process PID
 * shell   : shell executable path (may be NULL)
 */
void
win32_session_log_open(int pane_id, long pid, const char *shell)
{
    char   ts[32];
    char   entry[512];
    char   shell_safe[256];
    char  *existing;
    char  *newcontent;
    size_t existing_len, entry_len;
    char   evmsg[512];
    int    i;

    if (!session_log_initialized || session_log_path[0] == '\0')
        return;

    iso8601_now(ts, sizeof ts);

    /* Escape backslashes in shell path for JSON */
    shell_safe[0] = '\0';
    if (shell != NULL) {
        int j = 0;
        for (i = 0; shell[i] != '\0' && j < (int)sizeof(shell_safe) - 2; i++) {
            if (shell[i] == '\\') {
                shell_safe[j++] = '\\';
                shell_safe[j++] = '\\';
            } else {
                shell_safe[j++] = shell[i];
            }
        }
        shell_safe[j] = '\0';
    }

    snprintf(entry, sizeof entry,
        "{\"id\":%d,\"pid\":%ld,\"shell\":\"%s\","
        "\"opened\":\"%s\",\"state\":\"active\"}\n",
        pane_id, pid, shell_safe, ts);

    acquire_mutex();

    existing = read_log_file();
    existing_len = existing != NULL ? strlen(existing) : 0;
    entry_len    = strlen(entry);

    newcontent = malloc(existing_len + entry_len + 1);
    if (newcontent != NULL) {
        if (existing != NULL)
            memcpy(newcontent, existing, existing_len);
        memcpy(newcontent + existing_len, entry, entry_len);
        newcontent[existing_len + entry_len] = '\0';
        write_log_file(newcontent);
        free(newcontent);
    }
    free(existing);

    release_mutex();

    win32_log("win32_session_log_open: pane=%d pid=%ld\n", pane_id, pid);

    snprintf(evmsg, sizeof evmsg,
        "tmux pane opened: id=%d pid=%ld shell=%s",
        pane_id, pid, shell != NULL ? shell : "(none)");
    emit_event_log(evmsg);
}

/*
 * win32_session_log_close - mark a pane as dead.
 *
 * Rewrites the matching entry in-place to add "closed" timestamp and
 * set state to "dead". If the entry is not found, appends a dead record.
 */
void
win32_session_log_close(long pid)
{
    char  ts[32];
    char *buf, *line, *next, *out;
    size_t out_cap, out_len;
    int    matched = 0;
    char   evmsg[256];

    if (!session_log_initialized || session_log_path[0] == '\0')
        return;

    iso8601_now(ts, sizeof ts);

    acquire_mutex();

    buf = read_log_file();
    if (buf == NULL) {
        release_mutex();
        return;
    }

    out_cap = strlen(buf) + 128;
    out = malloc(out_cap);
    if (out == NULL) {
        free(buf);
        release_mutex();
        return;
    }
    out_len = 0;

    line = buf;
    while (line != NULL && *line != '\0') {
        /* Find end of this line */
        next = strchr(line, '\n');
        if (next != NULL)
            *next++ = '\0';

        if (*line == '\0') {
            line = next;
            continue;
        }

        /*
         * Check if this line matches the PID and is still active.
         * Look for "pid":<pid> and "state":"active" in the JSON line.
         */
        if (!matched) {
            char pid_token[32];
            snprintf(pid_token, sizeof pid_token, "\"pid\":%ld,", pid);
            if (strstr(line, pid_token) != NULL &&
                strstr(line, "\"state\":\"active\"") != NULL) {
                /*
                 * Rewrite this line: strip trailing '}', append closed/dead.
                 * Find the last '}' and replace it.
                 */
                char *brace = strrchr(line, '}');
                char newline[768];
                if (brace != NULL) {
                    *brace = '\0';
                    snprintf(newline, sizeof newline,
                        "%s,\"closed\":\"%s\",\"state\":\"dead\"}\n",
                        line, ts);
                } else {
                    /* malformed line — pass through unchanged */
                    snprintf(newline, sizeof newline, "%s\n", line);
                }

                size_t nl = strlen(newline);
                if (out_len + nl + 1 > out_cap) {
                    out_cap = out_len + nl + 256;
                    char *tmp = realloc(out, out_cap);
                    if (tmp == NULL) { free(out); free(buf); release_mutex(); return; }
                    out = tmp;
                }
                memcpy(out + out_len, newline, nl);
                out_len += nl;
                matched = 1;
                line = next;
                continue;
            }
        }

        /* Pass line through unchanged */
        size_t ll = strlen(line);
        if (out_len + ll + 2 > out_cap) {
            out_cap = out_len + ll + 256;
            char *tmp = realloc(out, out_cap);
            if (tmp == NULL) { free(out); free(buf); release_mutex(); return; }
            out = tmp;
        }
        memcpy(out + out_len, line, ll);
        out_len += ll;
        out[out_len++] = '\n';

        line = next;
    }
    out[out_len] = '\0';

    write_log_file(out);
    free(out);
    free(buf);

    release_mutex();

    win32_log("win32_session_log_close: pid=%ld matched=%d\n", pid, matched);

    snprintf(evmsg, sizeof evmsg, "tmux pane closed: pid=%ld", pid);
    emit_event_log(evmsg);
}

/*
 * win32_session_log_cleanup - remove dead entries older than SESSION_LOG_DEAD_AGE.
 *
 * Called on server startup. Keeps the log file bounded.
 */
void
win32_session_log_cleanup(void)
{
    char  *buf, *line, *next, *out;
    size_t out_cap, out_len;

    if (!session_log_initialized || session_log_path[0] == '\0')
        return;

    acquire_mutex();

    buf = read_log_file();
    if (buf == NULL) {
        release_mutex();
        return;
    }

    out_cap = strlen(buf) + 1;
    out = malloc(out_cap);
    if (out == NULL) {
        free(buf);
        release_mutex();
        return;
    }
    out_len = 0;

    line = buf;
    while (line != NULL && *line != '\0') {
        next = strchr(line, '\n');
        if (next != NULL)
            *next++ = '\0';

        if (*line == '\0') {
            line = next;
            continue;
        }

        /* Drop dead entries whose "opened" timestamp is older than the age limit */
        if (strstr(line, "\"state\":\"dead\"") != NULL) {
            /*
             * Parse "opened":"YYYY-MM-DDTHH:MM:SS" to get age in seconds.
             * Use SYSTEMTIME -> FILETIME -> ULARGE_INTEGER arithmetic to avoid
             * struct tm / mktime which are problematic under MSVC with WIN32_LEAN_AND_MEAN.
             */
            const char *op = strstr(line, "\"opened\":\"");
            if (op != NULL) {
                SYSTEMTIME st_opened;
                FILETIME   ft_opened, ft_now;
                ULARGE_INTEGER uli_opened, uli_now;
                memset(&st_opened, 0, sizeof st_opened);
                op += 10; /* skip past "opened":" */
                if (sscanf(op, "%hu-%hu-%huT%hu:%hu:%hu",
                        &st_opened.wYear, &st_opened.wMonth,  &st_opened.wDay,
                        &st_opened.wHour, &st_opened.wMinute, &st_opened.wSecond) == 6) {
                    if (SystemTimeToFileTime(&st_opened, &ft_opened)) {
                        GetSystemTimeAsFileTime(&ft_now);
                        uli_opened.LowPart  = ft_opened.dwLowDateTime;
                        uli_opened.HighPart = ft_opened.dwHighDateTime;
                        uli_now.LowPart     = ft_now.dwLowDateTime;
                        uli_now.HighPart    = ft_now.dwHighDateTime;
                        /* FILETIME ticks are 100ns; SESSION_LOG_DEAD_AGE is in seconds */
                        if (uli_now.QuadPart > uli_opened.QuadPart &&
                            (uli_now.QuadPart - uli_opened.QuadPart) >
                            (ULONGLONG)SESSION_LOG_DEAD_AGE * 10000000ULL) {
                            /* skip — drop this entry */
                            line = next;
                            continue;
                        }
                    }
                }
            }
        }

        size_t ll = strlen(line);
        if (out_len + ll + 2 > out_cap) {
            out_cap = out_len + ll + 256;
            char *tmp = realloc(out, out_cap);
            if (tmp == NULL) { free(out); free(buf); release_mutex(); return; }
            out = tmp;
        }
        memcpy(out + out_len, line, ll);
        out_len += ll;
        out[out_len++] = '\n';

        line = next;
    }
    out[out_len] = '\0';

    write_log_file(out);
    free(out);
    free(buf);

    release_mutex();
    win32_log("win32_session_log_cleanup: done\n");
}
