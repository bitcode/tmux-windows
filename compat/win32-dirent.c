/*
 * win32-dirent.c - Directory operations for Windows
 */

#include "win32.h"
#include "tmux-compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>

/*
 * opendir - open a directory for reading
 */
DIR *
opendir(const char *name)
{
    DIR *dir;
    char pattern[MAX_PATH];
    size_t len;

    if (name == NULL || name[0] == '\0') {
        errno = ENOENT;
        return NULL;
    }

    len = strlen(name);
    if (len >= MAX_PATH - 3) {
        errno = ENAMETOOLONG;
        return NULL;
    }

    dir = malloc(sizeof(DIR));
    if (dir == NULL) {
        errno = ENOMEM;
        return NULL;
    }

    /* Build search pattern */
    strcpy(pattern, name);
    if (pattern[len - 1] != '/' && pattern[len - 1] != '\\')
        strcat(pattern, "\\");
    strcat(pattern, "*");

    dir->handle = _findfirst(pattern, &dir->data);
    if (dir->handle == -1) {
        free(dir);
        errno = ENOENT;
        return NULL;
    }

    dir->first = 1;
    return dir;
}

/*
 * readdir - read next directory entry
 */
struct dirent *
readdir(DIR *dir)
{
    if (dir == NULL) {
        errno = EBADF;
        return NULL;
    }

    if (dir->first) {
        dir->first = 0;
    } else {
        if (_findnext(dir->handle, &dir->data) != 0)
            return NULL;
    }

    strncpy(dir->ent.d_name, dir->data.name, sizeof(dir->ent.d_name) - 1);
    dir->ent.d_name[sizeof(dir->ent.d_name) - 1] = '\0';

    if (dir->data.attrib & _A_SUBDIR)
        dir->ent.d_type = DT_DIR;
    else
        dir->ent.d_type = DT_REG;

    return &dir->ent;
}

/*
 * closedir - close directory
 */
int
closedir(DIR *dir)
{
    if (dir == NULL) {
        errno = EBADF;
        return -1;
    }

    if (dir->handle != -1)
        _findclose(dir->handle);

    free(dir);
    return 0;
}

/*
 * dirfd - get file descriptor for directory (stub)
 */
int
dirfd(DIR *dir)
{
    (void)dir;
    return -1;  /* Not supported on Windows */
}
