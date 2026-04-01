/*
 * win32-stubs.c - Stubs for missing POSIX functions to satisfy linker
 */

#ifndef _WINSOCK2API_
#include <winsock2.h>
#endif
#include <windows.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <time.h>

/* Missing environ */
/* extern char **environ; - defined in win32-socket.c */

/* Stubs */

/* fdforkpty is from libutil on BSD */
int
fdforkpty(int *master, int *slave, char *name, void *termp,
    void *winp)
{
    (void)master; (void)slave; (void)name; (void)termp; (void)winp;
    errno = ENOSYS;
    return -1;
}

char *
realpath(const char *path, char *resolved_path)
{
    if (path == NULL) {
        errno = EINVAL;
        return NULL;
    }
    if (resolved_path == NULL) {
        resolved_path = malloc(_MAX_PATH);
        if (resolved_path == NULL)
            return NULL;
    }
    /* Simple pass-through for now, or use _fullpath */
    if (_fullpath(resolved_path, path, _MAX_PATH) == NULL) {
        return NULL;
    }
    return resolved_path;
}

int
lstat(const char *path, struct stat *buf)
{
    /* Windows doesn't support symlinks like Unix, stat is close enough */
    return stat(path, buf);
}

void
setproctitle(const char *fmt, ...)
{
    (void)fmt;
    /* No-op on Windows */
}

int
tcflush(int fd, int queue_selector)
{
    (void)fd; (void)queue_selector;
    return 0;
}

int
getpagesize(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwPageSize;
}

/* Time function stubs to satisfy linker if CRT versions are missing/unresolved */

/* errno_t localtime_s(struct tm* _tm, const time_t *time); */
int
localtime_s(struct tm *_tm, const time_t *time)
{
    struct tm *res = localtime(time);
    if (res == NULL) return EINVAL;
    *_tm = *res;
    return 0;
}

/* errno_t gmtime_s(struct tm* _tm, const time_t *time); */
int
gmtime_s(struct tm *_tm, const time_t *time)
{
    struct tm *res = gmtime(time);
    if (res == NULL) return EINVAL;
    *_tm = *res;
    return 0;
}

/* errno_t ctime_s(char* buffer, size_t numberOfElements, const time_t *time); */
int
ctime_s(char* buffer, size_t numberOfElements, const time_t *time)
{
    char *res = ctime(time);
    if (res == NULL) return EINVAL;
    if (strlen(res) + 1 > numberOfElements) return ERANGE;
    strcpy(buffer, res);
    return 0;
}
