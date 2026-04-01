/*
 * win32-regex.c - Regular expression support for Windows
 *
 * This provides a minimal regex implementation.
 * For full functionality, link with PCRE2 library.
 */

#include "win32.h"
#include "tmux-compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/*
 * Simple regex implementation for basic patterns.
 * Supports: literal characters, ., *, ^, $
 * For complex patterns, use PCRE2.
 */

struct regex_internal {
    char *pattern;
    int flags;
};

/*
 * regcomp - compile a regular expression
 */
int
regcomp(regex_t *preg, const char *pattern, int cflags)
{
    struct regex_internal *ri;

    if (preg == NULL || pattern == NULL)
        return REG_NOMATCH;

    ri = malloc(sizeof(struct regex_internal));
    if (ri == NULL)
        return REG_NOMATCH;

    ri->pattern = _strdup(pattern);
    if (ri->pattern == NULL) {
        free(ri);
        return REG_NOMATCH;
    }

    ri->flags = cflags;
    preg->re_pcre = ri;
    preg->re_nsub = 0;

    /* Count capture groups */
    const char *p = pattern;
    while (*p) {
        if (*p == '(' && (p == pattern || p[-1] != '\\'))
            preg->re_nsub++;
        p++;
    }

    return 0;
}

/*
 * Simple pattern matching
 */
static int
simple_match(const char *pattern, const char *string, int icase)
{
    while (*pattern && *string) {
        if (*pattern == '.') {
            /* Match any character */
            pattern++;
            string++;
        } else if (*pattern == '*') {
            /* Match zero or more of previous (simplified) */
            pattern++;
            if (*pattern == '\0')
                return 1;  /* .* at end matches everything */
            while (*string) {
                if (simple_match(pattern, string, icase))
                    return 1;
                string++;
            }
            return simple_match(pattern, string, icase);
        } else if (*pattern == '\\' && pattern[1]) {
            /* Escaped character */
            pattern++;
            if (icase) {
                if (tolower((unsigned char)*pattern) != tolower((unsigned char)*string))
                    return 0;
            } else {
                if (*pattern != *string)
                    return 0;
            }
            pattern++;
            string++;
        } else {
            /* Literal match */
            if (icase) {
                if (tolower((unsigned char)*pattern) != tolower((unsigned char)*string))
                    return 0;
            } else {
                if (*pattern != *string)
                    return 0;
            }
            pattern++;
            string++;
        }
    }

    /* Handle trailing .* */
    while (*pattern == '*' || *pattern == '.')
        pattern++;

    return (*pattern == '\0');
}

/*
 * regexec - execute a compiled regular expression
 */
int
regexec(const regex_t *preg, const char *string, size_t nmatch,
        regmatch_t pmatch[], int eflags)
{
    struct regex_internal *ri;
    const char *pattern;
    const char *s;
    int icase;
    int notbol, noteol;

    if (preg == NULL || preg->re_pcre == NULL || string == NULL)
        return REG_NOMATCH;

    ri = preg->re_pcre;
    pattern = ri->pattern;
    icase = (ri->flags & REG_ICASE) != 0;
    notbol = (eflags & REG_NOTBOL) != 0;
    noteol = (eflags & REG_NOTEOL) != 0;

    (void)notbol;  /* TODO: handle */
    (void)noteol;

    /* Handle ^ anchor */
    if (pattern[0] == '^') {
        pattern++;
        if (simple_match(pattern, string, icase)) {
            if (pmatch && nmatch > 0) {
                pmatch[0].rm_so = 0;
                pmatch[0].rm_eo = (regoff_t)strlen(string);
            }
            return 0;
        }
        return REG_NOMATCH;
    }

    /* Try matching at each position */
    for (s = string; *s; s++) {
        const char *p = pattern;
        const char *end = s;
        int matched = 0;

        /* Handle $ anchor */
        size_t plen = strlen(p);
        if (plen > 0 && p[plen - 1] == '$') {
            /* Match at end only */
            char *pcopy = _strdup(p);
            if (pcopy) {
                pcopy[plen - 1] = '\0';
                size_t slen = strlen(s);
                size_t pcopylen = strlen(pcopy);
                if (slen >= pcopylen) {
                    matched = simple_match(pcopy, s + slen - pcopylen, icase);
                    if (matched)
                        end = s + slen;
                }
                free(pcopy);
            }
        } else {
            /* Find longest match */
            const char *t = s;
            while (*t) {
                t++;
                /* Try progressively longer substrings */
                char saved = *t;
                *(char *)t = '\0';
                if (simple_match(p, s, icase)) {
                    matched = 1;
                    end = t;
                }
                *(char *)t = saved;
            }
            /* Also try full remaining string */
            if (simple_match(p, s, icase)) {
                matched = 1;
                end = s + strlen(s);
            }
        }

        if (matched) {
            if (pmatch && nmatch > 0) {
                pmatch[0].rm_so = (regoff_t)(s - string);
                pmatch[0].rm_eo = (regoff_t)(end - string);
            }
            return 0;
        }
    }

    return REG_NOMATCH;
}

/*
 * regfree - free a compiled regular expression
 */
void
regfree(regex_t *preg)
{
    if (preg && preg->re_pcre) {
        struct regex_internal *ri = preg->re_pcre;
        free(ri->pattern);
        free(ri);
        preg->re_pcre = NULL;
    }
}

/*
 * regerror - get error message
 */
size_t
regerror(int errcode, const regex_t *preg, char *errbuf, size_t errbuf_size)
{
    const char *msg;

    (void)preg;

    switch (errcode) {
    case 0:
        msg = "Success";
        break;
    case REG_NOMATCH:
        msg = "No match";
        break;
    default:
        msg = "Unknown error";
        break;
    }

    if (errbuf && errbuf_size > 0) {
        strncpy(errbuf, msg, errbuf_size - 1);
        errbuf[errbuf_size - 1] = '\0';
    }

    return strlen(msg) + 1;
}
