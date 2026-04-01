#ifndef _GLOB_H
#define _GLOB_H

#include "win32.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
    size_t gl_pathc;
    char **gl_pathv;
    size_t gl_offs;
} glob_t;

#define GLOB_NOSORT   0x04
#define GLOB_MARK     0x08
#define GLOB_NOSPACE  1
#define GLOB_ABORTED  2
#define GLOB_NOMATCH  3

/*
 * Minimal glob() for Windows.
 *
 * Handles the common tmux use case: source-file <absolute-path> where the
 * path contains no wildcard characters.  Falls back to FindFirstFile for
 * simple single-directory patterns.
 */
static inline int
glob(const char *pattern, int flags, int (*errfunc)(const char *, int),
    glob_t *pglob)
{
    (void)flags; (void)errfunc;

    pglob->gl_pathc = 0;
    pglob->gl_pathv = NULL;
    pglob->gl_offs  = 0;

    if (pattern == NULL || pattern[0] == '\0')
        return GLOB_NOMATCH;

    /* If no wildcard characters, test for exact file existence */
    if (strchr(pattern, '*') == NULL && strchr(pattern, '?') == NULL &&
        strchr(pattern, '[') == NULL) {
        /*
         * Translate the path before stat so that Unix-style paths like
         * /tmp/foo get mapped to the correct Windows location.
         */
        const char *check_path = win32_translate_path(pattern);
        DWORD attrs = GetFileAttributesA(check_path);
        if (attrs == INVALID_FILE_ATTRIBUTES)
            return GLOB_NOMATCH;
        pglob->gl_pathv = (char **)malloc(2 * sizeof(char *));
        if (pglob->gl_pathv == NULL)
            return GLOB_NOSPACE;
        /* Return the translated path so file_get_path gets a Windows path */
        pglob->gl_pathv[0] = _strdup(check_path);
        if (pglob->gl_pathv[0] == NULL) {
            free(pglob->gl_pathv);
            pglob->gl_pathv = NULL;
            return GLOB_NOSPACE;
        }
        pglob->gl_pathv[1] = NULL;
        pglob->gl_pathc    = 1;
        return 0;
    }

    /* Wildcard case: use FindFirstFile */
    {
        WIN32_FIND_DATAA ffd;
        HANDLE hFind;
        char **paths = NULL;
        size_t count = 0, cap = 0;
        char dir[MAX_PATH];
        const char *last_sep;
        size_t dir_len = 0;

        /* Extract directory prefix */
        last_sep = strrchr(pattern, '/');
        if (last_sep == NULL)
            last_sep = strrchr(pattern, '\\');
        if (last_sep != NULL) {
            dir_len = (size_t)(last_sep - pattern) + 1;
            if (dir_len >= MAX_PATH)
                return GLOB_ABORTED;
            memcpy(dir, pattern, dir_len);
            dir[dir_len] = '\0';
        }

        hFind = FindFirstFileA(pattern, &ffd);
        if (hFind == INVALID_HANDLE_VALUE)
            return GLOB_NOMATCH;

        do {
            char full[MAX_PATH];
            char **tmp;

            if (strcmp(ffd.cFileName, ".") == 0 ||
                strcmp(ffd.cFileName, "..") == 0)
                continue;

            if (dir_len > 0)
                snprintf(full, sizeof full, "%.*s%s",
                    (int)dir_len, dir, ffd.cFileName);
            else
                snprintf(full, sizeof full, "%s", ffd.cFileName);

            if (count >= cap) {
                cap = cap ? cap * 2 : 8;
                tmp = (char **)realloc(paths, (cap + 1) * sizeof(char *));
                if (tmp == NULL) {
                    for (size_t i = 0; i < count; i++) free(paths[i]);
                    free(paths);
                    FindClose(hFind);
                    return GLOB_NOSPACE;
                }
                paths = tmp;
            }
            paths[count] = _strdup(full);
            if (paths[count] == NULL) {
                for (size_t i = 0; i < count; i++) free(paths[i]);
                free(paths);
                FindClose(hFind);
                return GLOB_NOSPACE;
            }
            count++;
        } while (FindNextFileA(hFind, &ffd));

        FindClose(hFind);

        if (count == 0) {
            free(paths);
            return GLOB_NOMATCH;
        }

        paths[count] = NULL;
        pglob->gl_pathv = paths;
        pglob->gl_pathc = count;
        return 0;
    }
}

static inline void
globfree(glob_t *pglob)
{
    size_t i;
    if (pglob->gl_pathv != NULL) {
        for (i = 0; i < pglob->gl_pathc; i++)
            free(pglob->gl_pathv[i]);
        free(pglob->gl_pathv);
        pglob->gl_pathv = NULL;
    }
    pglob->gl_pathc = 0;
}

#endif
