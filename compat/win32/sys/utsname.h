#ifndef _SYS_UTSNAME_H
#define _SYS_UTSNAME_H

#include "win32.h"

struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
};

static inline int uname(struct utsname *name) {
    if (name == NULL) return -1;
    strncpy(name->sysname, "Windows", sizeof(name->sysname));
    strncpy(name->nodename, "localhost", sizeof(name->nodename));
    strncpy(name->release, "10.0", sizeof(name->release));
    strncpy(name->version, "10.0", sizeof(name->version));
    strncpy(name->machine, "x86_64", sizeof(name->machine));
    return 0;
}

#endif
