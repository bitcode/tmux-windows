/*
 * fnmatch.h - Filename pattern matching for Windows
 */

#ifndef COMPAT_WIN32_FNMATCH_H
#define COMPAT_WIN32_FNMATCH_H

/* Flags */
#define FNM_NOMATCH     1       /* Match failed */
#define FNM_NOESCAPE    0x01    /* Disable backslash escaping */
#define FNM_PATHNAME    0x02    /* Slash must be matched by slash */
#define FNM_PERIOD      0x04    /* Period must be matched by period */
#define FNM_LEADING_DIR 0x08    /* Ignore /... after match */
#define FNM_CASEFOLD    0x10    /* Case-insensitive matching */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * fnmatch - match filename against pattern
 *
 * Returns 0 on match, FNM_NOMATCH on no match, -1 on error.
 */
int fnmatch(const char *pattern, const char *string, int flags);

#ifdef __cplusplus
}
#endif

#endif /* COMPAT_WIN32_FNMATCH_H */
