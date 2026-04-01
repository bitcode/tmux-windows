/*
 * win32-misc.c - Miscellaneous compatibility functions
 */

#include "tmux.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef getenv
#undef getenv
#endif

extern char *xstrdup(const char *);

static SRWLOCK log_srw = SRWLOCK_INIT;

void
win32_log(const char *fmt, ...)
{
    char buf[4096];
    va_list ap;
    char logfile[MAX_PATH];
    DWORD pid = GetCurrentProcessId();
    SYSTEMTIME st;
    int len, msg_len;

    memset(buf, 0, sizeof(buf));
    GetLocalTime(&st);
    snprintf(logfile, sizeof(logfile), "debug-%lu.log", (unsigned long)pid);

    AcquireSRWLockExclusive(&log_srw);

    /* Format timestamp and PID */
    len = _snprintf(buf, sizeof(buf) - 2, "[%02d:%02d:%02d.%03u] [%lu] ",
                  st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, (unsigned long)pid);
    if (len < 0) len = 0;

    /* Format message */
    va_start(ap, fmt);
    msg_len = _vsnprintf(buf + len, sizeof(buf) - len - 2, fmt, ap);
    va_end(ap);

    if (msg_len < 0) {
        /* If truncated, ensure we don't skip the rest of the buffer */
        len = strlen(buf);
    } else {
        len += msg_len;
    }

    if (len > 0 && len < sizeof(buf) - 2 && buf[len-1] != '\n') {
        buf[len++] = '\n';
        buf[len] = '\0';
    }

    /* Use WriteFile for thread-safety and to avoid stdio buffering issues */
    HANDLE hFile = CreateFileA(logfile, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(hFile, buf, (DWORD)len, &written, NULL);
        CloseHandle(hFile);
    }

    ReleaseSRWLockExclusive(&log_srw);
}



size_t
win32_strftime_safe(char *s, size_t maxsize, const char *format, const struct tm *tm)
{
	const char	*p;
	char		*fmt_copy, *q;
	size_t		 r;

	if (tm == NULL) {
		win32_log("win32_strftime_safe: NULL tm detected\n");
		return (0);
	}

	/* 
	 * Windows strftime crashes if tm members are invalid.
	 * tm_mday must be 1-31, tm_mon 0-11.
	 */
	if (tm->tm_mday < 1 || tm->tm_mday > 31 ||
	    tm->tm_mon < 0 || tm->tm_mon > 11 ||
	    tm->tm_year < 0 || tm->tm_year > 8000) {
		win32_log("win32_strftime_safe: invalid tm detected (mday=%d mon=%d year=%d)\n",
			tm->tm_mday, tm->tm_mon, tm->tm_year);
		return (0);
	}

	/*
	 * Windows strftime crashes if it encounters an invalid % specifier.
	 * We'll make a copy and scrub any suspicious % sequences.
	 */
	fmt_copy = xstrdup(format);
	q = fmt_copy;
	while ((q = strchr(q, '%')) != NULL) {
		q++;
		if (*q == '\0') {
			*(q - 1) = ' '; /* Trailing % */
			break;
		}
		if (*q == '%') {
			q++;
			continue;
		}
		/* Valid specifiers: aAbBcdHIjmMpSUwWxXyYzZ% and # plus some of them. */
		if (strchr("aAbBcdHIjmMpSUwWxXyYzZ#", *q) == NULL) {
			*(q - 1) = '_'; /* Replace problematic % */
		}
		q++;
	}

#undef strftime
	r = strftime(s, maxsize, fmt_copy, tm);
	free(fmt_copy);
	return (r);
}

/*
 * clock_gettime - get time from specified clock
 */
int
win32_clock_gettime(int clock_id, struct timespec *tp)
{
    FILETIME ft;
    ULARGE_INTEGER ul;
    static LARGE_INTEGER freq = {0};
    LARGE_INTEGER counter;

    if (tp == NULL) {
        errno = EINVAL;
        return -1;
    }

    switch (clock_id) {
    case CLOCK_REALTIME:
        GetSystemTimeAsFileTime(&ft);
        ul.LowPart = ft.dwLowDateTime;
        ul.HighPart = ft.dwHighDateTime;
        /* Convert from 100-ns intervals since 1601 to Unix epoch */
        ul.QuadPart -= 116444736000000000ULL;
        tp->tv_sec = (time_t)(ul.QuadPart / 10000000);
        tp->tv_nsec = (long)((ul.QuadPart % 10000000) * 100);
        break;

    case CLOCK_MONOTONIC:
        if (freq.QuadPart == 0) {
            QueryPerformanceFrequency(&freq);
        }
        QueryPerformanceCounter(&counter);
        tp->tv_sec = (time_t)(counter.QuadPart / freq.QuadPart);
        tp->tv_nsec = (long)(((counter.QuadPart % freq.QuadPart) * 1000000000) /
                             freq.QuadPart);
        break;

    default:
        errno = EINVAL;
        return -1;
    }

    return 0;
}

/*
 * nanosleep - high resolution sleep
 */
int
win32_nanosleep(const struct timespec *req, struct timespec *rem)
{
    DWORD ms;

    if (req == NULL) {
        errno = EINVAL;
        return -1;
    }

    ms = (DWORD)(req->tv_sec * 1000 + req->tv_nsec / 1000000);
    if (ms == 0 && (req->tv_sec > 0 || req->tv_nsec > 0))
        ms = 1;

    Sleep(ms);

    if (rem != NULL) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }

    return 0;
}

/*
 * usleep - microsecond sleep
 */
int
win32_usleep(unsigned int usec)
{
    DWORD ms = usec / 1000;
    if (ms == 0 && usec > 0)
        ms = 1;
    Sleep(ms);
    return 0;
}

/*
 * sleep - second sleep
 */
unsigned int
win32_sleep(unsigned int seconds)
{
    Sleep(seconds * 1000);
    return 0;
}

/*
 * setenv - set environment variable
 */
int
win32_setenv(const char *name, const char *value, int overwrite)
{
    if (name == NULL || name[0] == '\0' || strchr(name, '=') != NULL) {
        errno = EINVAL;
        return -1;
    }

    if (!overwrite && getenv(name) != NULL)
        return 0;

    if (_putenv_s(name, value) != 0) {
        errno = ENOMEM;
        return -1;
    }

    return 0;
}

/*
 * unsetenv - remove environment variable
 */
int
win32_unsetenv(const char *name)
{
    if (name == NULL || name[0] == '\0' || strchr(name, '=') != NULL) {
        errno = EINVAL;
        return -1;
    }

    _putenv_s(name, "");
    return 0;
}

/*
 * Error handling
 */
void
win32_perror(const char *msg)
{
    DWORD err = GetLastError();
    char *buf = NULL;

    FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, err, 0, (LPSTR)&buf, 0, NULL);

    if (msg && msg[0])
        fprintf(stderr, "%s: %s", msg, buf ? buf : "Unknown error");
    else
        fprintf(stderr, "%s", buf ? buf : "Unknown error");

    if (buf)
        LocalFree(buf);
}

char *
win32_strerror(int errnum)
{
    static char buf[256];

    if (errnum < 0) {
        /* Might be a Windows error code */
        FormatMessageA(
            FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
            NULL, (DWORD)(-errnum), 0, buf, sizeof(buf), NULL);
        return buf;
    }

    return strerror(errnum);
}

/*
 * Path translation helpers
 */
char *
win32_translate_path(const char *path)
{
    static char buf[MAX_PATH];
    char *p;

    if (path == NULL)
        return NULL;

    /* Already a Windows path */
    if (strchr(path, '\\') != NULL || (path[0] && path[1] == ':'))
        return (char *)path;

    /* Handle common Unix paths */
    if (strncmp(path, "/tmp/", 5) == 0) {
        const char *tmp = getenv("TEMP");
        if (tmp == NULL)
            tmp = getenv("TMP");
        if (tmp == NULL)
            tmp = "C:\\Windows\\Temp";
        snprintf(buf, sizeof(buf), "%s\\%s", tmp, path + 5);
    } else if (strncmp(path, "/dev/null", 9) == 0) {
        strcpy(buf, "NUL");
    } else if (strncmp(path, "/dev/tty", 8) == 0) {
        strcpy(buf, "CONIN$");
    } else if (path[0] == '~') {
        const char *home = getenv("USERPROFILE");
        if (home == NULL)
            home = "C:\\Users\\Default";
        snprintf(buf, sizeof(buf), "%s%s", home, path + 1);
    } else {
        /* Copy and convert slashes */
        strncpy(buf, path, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
    }

    /* Convert forward slashes to backslashes */
    for (p = buf; *p; p++) {
        if (*p == '/')
            *p = '\\';
    }

    return buf;
}

/*
 * Get environment variable with path translation
 */
char *
win32_getenv_path(const char *name)
{
    char *value = getenv(name);

    if (value == NULL)
        return NULL;

    return win32_translate_path(value);
}

/*
 * fnmatch replacement (simple glob matching)
 * Returns 0 on match, FNM_NOMATCH (1) on no match
 */
#ifndef FNM_NOMATCH
#define FNM_NOMATCH   1
#define FNM_CASEFOLD  0x10
#endif

int
fnmatch(const char *pattern, const char *string, int flags)
{
    int icase = (flags & FNM_CASEFOLD) != 0;

    while (*pattern && *string) {
        if (*pattern == '*') {
            pattern++;
            if (*pattern == '\0')
                return 0;
            while (*string) {
                if (fnmatch(pattern, string, flags) == 0)
                    return 0;
                string++;
            }
            return FNM_NOMATCH;
        } else if (*pattern == '?') {
            pattern++;
            string++;
        } else {
            int pc = (unsigned char)*pattern;
            int sc = (unsigned char)*string;
            if (icase) {
                pc = tolower(pc);
                sc = tolower(sc);
            }
            if (pc != sc)
                return FNM_NOMATCH;
            pattern++;
            string++;
        }
    }

    while (*pattern == '*')
        pattern++;

    return (*pattern == '\0' && *string == '\0') ? 0 : FNM_NOMATCH;
}

/*
 * basename - return last component of a pathname
 */
char *
win32_basename(char *path)
{
    static char buf[MAX_PATH];
    char *p;

    if (path == NULL || *path == '\0') {
        strcpy(buf, ".");
        return buf;
    }

    /* Convert backslashes for processing */
    for (p = path; *p; p++) {
        if (*p == '\\') *p = '/';
    }

    p = strrchr(path, '/');
    if (p == NULL)
        return path;

    if (*(p + 1) == '\0') {
        /* Ends in slash, needs more complex handling for POSIX parity */
        *p = '\0';
        p = strrchr(path, '/');
        if (p == NULL) return path;
    }

    return p + 1;
}

/*
 * dirname - return directory component of a pathname
 */
char *
win32_dirname(char *path)
{
    static char buf[MAX_PATH];
    char *p;

    if (path == NULL || *path == '\0') {
        strcpy(buf, ".");
        return buf;
    }

    /* Convert backslashes */
    for (p = path; *p; p++) {
        if (*p == '\\') *p = '/';
    }

    p = strrchr(path, '/');
    if (p == NULL) {
        strcpy(buf, ".");
        return buf;
    }

    if (p == path) {
        strcpy(buf, "/");
        return buf;
    }

    size_t len = p - path;
    strncpy(buf, path, len);
    buf[len] = '\0';
    return buf;
}

/*
 * pwd.h compatibility - minimal implementation
 */
static struct passwd win32_pw;

struct passwd *
win32_getpwuid(uid_t uid)
{
    static char name[LOGIN_NAME_MAX];
    static char dir[MAX_PATH];
    DWORD nsize = sizeof(name);
    
    (void)uid;

    if (GetUserNameA(name, &nsize)) {
        win32_pw.pw_name = name;
        win32_pw.pw_passwd = "*";
        win32_pw.pw_uid = 1000;
        win32_pw.pw_gid = 1000;
        win32_pw.pw_gecos = name;
        
        const char *home = getenv("USERPROFILE");
        if (home) {
            strncpy(dir, home, sizeof(dir)-1);
            win32_pw.pw_dir = dir;
        } else {
            win32_pw.pw_dir = "C:\\";
        }
        win32_pw.pw_shell = "cmd.exe";
        return &win32_pw;
    }
    return NULL;
}

struct passwd *
win32_getpwnam(const char *name)
{
    return win32_getpwuid(0);
}

/*
 * mkstemp implementation
 */
int
mkstemp(char *template)
{
    errno_t err;
    if (template == NULL) {
        errno = EINVAL;
        return -1;
    }

    err = _mktemp_s(template, strlen(template) + 1);
    if (err != 0) {
        errno = err;
        return -1;
    }

    return _open(template, _O_RDWR | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
}

/*
 * unlink - delete a file
 */
int
win32_unlink(const char *path)
{
    char *p;
    
    if (path == NULL) {
        errno = EINVAL;
        return -1;
    }

    p = win32_translate_path(path);
    return _unlink(p);
}

/*
 * rmdir - delete a directory
 */
int
win32_rmdir(const char *path)
{
    char *p;

    if (path == NULL) {
        errno = EINVAL;
        return -1;
    }

    p = win32_translate_path(path);
    return _rmdir(p);
}

