/*
 * sys/un.h - Unix domain socket definitions for Windows
 *
 * Windows 10 version 1803+ supports AF_UNIX sockets.
 * The afunix.h header provides the sockaddr_un structure.
 */

#ifndef COMPAT_WIN32_SYS_UN_H
#define COMPAT_WIN32_SYS_UN_H

#include <winsock2.h>
#include <afunix.h>

/* Ensure AF_UNIX is defined */
#ifndef AF_UNIX
#define AF_UNIX 1
#endif

#ifndef AF_LOCAL
#define AF_LOCAL AF_UNIX
#endif

/* sockaddr_un should be defined in afunix.h */
/* If not available, define it here */
#ifndef UNIX_PATH_MAX
#define UNIX_PATH_MAX 108

struct sockaddr_un {
    ADDRESS_FAMILY sun_family;
    char sun_path[UNIX_PATH_MAX];
};
#endif

/* SUN_LEN macro */
#ifndef SUN_LEN
#define SUN_LEN(su) \
    (sizeof(*(su)) - sizeof((su)->sun_path) + strlen((su)->sun_path))
#endif

#endif /* COMPAT_WIN32_SYS_UN_H */
