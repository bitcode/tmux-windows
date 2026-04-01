/*
 * unistd.h - POSIX unistd.h replacement for Windows
 */

#ifndef COMPAT_WIN32_UNISTD_H
#define COMPAT_WIN32_UNISTD_H

#include <io.h>
#include <process.h>
#include <direct.h>
#include <stdint.h>

/* Include the master win32 compat header for type definitions */
#include "win32.h"

/* Standard file descriptors */
#ifndef STDIN_FILENO
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#endif

/* access() mode flags */
#ifndef F_OK
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4
#endif

/* lseek whence values - usually defined in stdio.h on Windows */
#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

/* sysconf names */
#define _SC_OPEN_MAX        4
#define _SC_PAGESIZE        30
#define _SC_PAGE_SIZE       _SC_PAGESIZE
#define _SC_CLK_TCK         2
#define _SC_HOST_NAME_MAX   180

/* pathconf names */
#define _PC_PATH_MAX        4
#define _PC_NAME_MAX        3

/* confstr names */
#define _CS_PATH            0

#ifdef __cplusplus
extern "C" {
#endif

/* sysconf implementation */
static inline long sysconf(int name) {
    switch (name) {
        case _SC_OPEN_MAX:
            return 2048;
        case _SC_PAGESIZE:
            {
                SYSTEM_INFO si;
                GetSystemInfo(&si);
                return si.dwPageSize;
            }
        case _SC_CLK_TCK:
            return 1000;  /* milliseconds */
        case _SC_HOST_NAME_MAX:
            return HOST_NAME_MAX;
        default:
            return -1;
    }
}

/* pathconf implementation */
static inline long pathconf(const char *path, int name) {
    (void)path;
    switch (name) {
        case _PC_PATH_MAX:
            return PATH_MAX;
        case _PC_NAME_MAX:
            return NAME_MAX;
        default:
            return -1;
    }
}

/* win32_gethostname - available in Winsock, renaming to avoid collision */
static inline int win32_gethostname(char *name, size_t len) {
    /* Use local undef to avoid recursion with win32.h macro */
#ifdef gethostname
#undef gethostname
#endif
    return gethostname(name, (int)len);
}

/* getlogin - return username */
static inline char *getlogin(void) {
    static char username[LOGIN_NAME_MAX];
    DWORD size = sizeof(username);
    if (GetUserNameA(username, &size))
        return username;
    return NULL;
}

#ifdef __cplusplus
}
#endif

#endif /* COMPAT_WIN32_UNISTD_H */
