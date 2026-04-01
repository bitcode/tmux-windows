/*
 * sys/time.h - Time definitions for Windows
 */

#ifndef COMPAT_WIN32_SYS_TIME_H
#define COMPAT_WIN32_SYS_TIME_H

#include <winsock2.h>  /* For struct timeval */
#include <time.h>

/* timeval is defined in winsock2.h */

#ifndef timersub
#define timersub(tvp, uvp, vvp)                                         \
    do {                                                                \
        (vvp)->tv_sec = (tvp)->tv_sec - (uvp)->tv_sec;                  \
        (vvp)->tv_usec = (tvp)->tv_usec - (uvp)->tv_usec;               \
        if ((vvp)->tv_usec < 0) {                                       \
            (vvp)->tv_sec--;                                            \
            (vvp)->tv_usec += 1000000;                                  \
        }                                                               \
    } while (0)
#endif

#ifndef timeradd
#define timeradd(tvp, uvp, vvp)                                         \
    do {                                                                \
        (vvp)->tv_sec = (tvp)->tv_sec + (uvp)->tv_sec;                  \
        (vvp)->tv_usec = (tvp)->tv_usec + (uvp)->tv_usec;               \
        if ((vvp)->tv_usec >= 1000000) {                                \
            (vvp)->tv_sec++;                                            \
            (vvp)->tv_usec -= 1000000;                                  \
        }                                                               \
    } while (0)
#endif

#ifndef timercmp
#define timercmp(tvp, uvp, cmp)                                         \
    (((tvp)->tv_sec == (uvp)->tv_sec) ?                                 \
        ((tvp)->tv_usec cmp (uvp)->tv_usec) :                           \
        ((tvp)->tv_sec cmp (uvp)->tv_sec))
#endif

#ifndef timerclear
#define timerclear(tvp) ((tvp)->tv_sec = (tvp)->tv_usec = 0)
#endif

#ifndef timerisset
#define timerisset(tvp) ((tvp)->tv_sec || (tvp)->tv_usec)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* gettimeofday */
#ifndef _GETTIMEOFDAY_DEFINED
#define _GETTIMEOFDAY_DEFINED
static inline int gettimeofday(struct timeval *tv, void *tz) {
    FILETIME ft;
    ULARGE_INTEGER ul;
    (void)tz;
    if (tv == NULL) return -1;
    GetSystemTimeAsFileTime(&ft);
    ul.LowPart = ft.dwLowDateTime;
    ul.HighPart = ft.dwHighDateTime;
    ul.QuadPart -= 116444736000000000ULL;
    tv->tv_sec = (long)(ul.QuadPart / 10000000);
    tv->tv_usec = (long)((ul.QuadPart % 10000000) / 10);
    return 0;
}
#endif

#ifdef __cplusplus
}
#endif

#endif /* COMPAT_WIN32_SYS_TIME_H */
