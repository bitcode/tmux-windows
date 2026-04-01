/*
 * win32-regex.h - Regular expression support for Windows
 */

#ifndef COMPAT_WIN32_REGEX_H
#define COMPAT_WIN32_REGEX_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Regex flags */
#define REG_EXTENDED 0x01
#define REG_ICASE    0x02
#define REG_NOSUB    0x04
#define REG_NEWLINE  0x08
#define REG_NOTBOL   0x10
#define REG_NOTEOL   0x20

/* Match offset type */
typedef int regoff_t;

/* Match result structure */
typedef struct {
    regoff_t rm_so;
    regoff_t rm_eo;
} regmatch_t;

/* Regex compilation structure */
typedef struct {
    size_t re_nsub;
    void  *re_pcre;
} regex_t;

/* Error codes */
#define REG_NOMATCH 1
#define REG_BADPAT  2
#define REG_ECOLLATE 3
#define REG_ECTYPE  4
#define REG_EESCAPE 5
#define REG_ESUBREG 6
#define REG_EBRACK  7
#define REG_EPAREN  8
#define REG_EBRACE  9
#define REG_BADBR   10
#define REG_ERANGE  11
#define REG_ESPACE  12
#define REG_BADRPT  13

int    regcomp(regex_t *, const char *, int);
int    regexec(const regex_t *, const char *, size_t, regmatch_t [], int);
void   regfree(regex_t *);
size_t regerror(int, const regex_t *, char *, size_t);

#ifdef __cplusplus
}
#endif

#endif /* COMPAT_WIN32_REGEX_H */
