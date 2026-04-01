/*
 * tmux-compat.h - Compatibility wrapper for tmux source files
 *
 * This header must be included BEFORE any tmux headers.
 * It sets up the Windows compatibility layer.
 */

#ifndef TMUX_COMPAT_H
#define TMUX_COMPAT_H

/* Platform detection */
#ifdef _WIN32
#define PLATFORM_WINDOWS 1
#endif

#ifdef PLATFORM_WINDOWS

/* Windows compatibility layer */
#include "win32.h"
#include "win32/sys/uio.h"

/* Ensure we have config.h definitions */
#include "config.h"

/*
 * Redirect some tmux-specific includes to our compat versions
 */

/* Block problematic POSIX headers that don't exist on Windows */
#define _SYS_WAIT_H_    /* Block sys/wait.h */
#define _SYS_UN_H_      /* Block sys/un.h */
#define _TERMIOS_H_     /* Block termios.h */

/* Provide replacements via our headers */
#include "win32/sys/wait.h"
#include "win32/sys/un.h"
#include "win32/termios.h"

/*
 * Additional type definitions that tmux expects
 */

/* Event loop - libevent types */
struct event;
struct event_base;
struct bufferevent;
struct evbuffer;

/*
 * Function-like macros that may need adjustment
 */

/* pledge() is OpenBSD-specific, no-op elsewhere */
#ifndef pledge
#define pledge(promises, execpromises) (0)
#endif

/* unveil() is OpenBSD-specific, no-op elsewhere */
#ifndef unveil
#define unveil(path, permissions) (0)
#endif

/*
 * tmux uses these for error handling
 */
#ifndef __dead
#define __dead __declspec(noreturn)
#endif

#ifndef __unused
#define __unused
#endif

#ifndef __packed
#define __packed
#endif

/*
 * Byte order macros
 */
#ifndef __BYTE_ORDER__
#define __BYTE_ORDER__ __ORDER_LITTLE_ENDIAN__
#define __ORDER_LITTLE_ENDIAN__ 1234
#define __ORDER_BIG_ENDIAN__ 4321
#endif

#ifndef htobe64
#define htobe64(x) _byteswap_uint64(x)
#define htole64(x) (x)
#define be64toh(x) _byteswap_uint64(x)
#define le64toh(x) (x)
#define htobe32(x) _byteswap_ulong(x)
#define htole32(x) (x)
#define be32toh(x) _byteswap_ulong(x)
#define le32toh(x) (x)
#define htobe16(x) _byteswap_ushort(x)
#define htole16(x) (x)
#define be16toh(x) _byteswap_ushort(x)
#define le16toh(x) (x)
#endif

/*
 * va_copy if not defined
 */
#ifndef va_copy
#define va_copy(dest, src) ((dest) = (src))
#endif

/*
 * Regex support - use PCRE or Windows regex
 */
#ifndef REG_EXTENDED
#define REG_EXTENDED 1
#define REG_ICASE    2
#define REG_NEWLINE  4
#define REG_NOSUB    8
#define REG_NOTBOL   16
#define REG_NOTEOL   32
#define REG_NOMATCH  1
typedef struct {
    void *re_pcre;
    size_t re_nsub;
} regex_t;
typedef int regoff_t;
typedef struct {
    regoff_t rm_so;
    regoff_t rm_eo;
} regmatch_t;
int regcomp(regex_t *preg, const char *pattern, int cflags);
int regexec(const regex_t *preg, const char *string, size_t nmatch,
            regmatch_t pmatch[], int eflags);
void regfree(regex_t *preg);
size_t regerror(int errcode, const regex_t *preg, char *errbuf, size_t errbuf_size);
#endif

/*
 * Directory entry
 */
#ifndef _DIRENT_H
#define _DIRENT_H
#include <io.h>
#include <direct.h>

struct dirent {
    char d_name[260];
    unsigned char d_type;
};

#define DT_UNKNOWN 0
#define DT_DIR     4
#define DT_REG     8

typedef struct {
    intptr_t handle;
    struct _finddata_t data;
    struct dirent ent;
    int first;
} DIR;

DIR *opendir(const char *name);
struct dirent *readdir(DIR *dir);
int closedir(DIR *dir);
#endif

#else /* !PLATFORM_WINDOWS */

/* On Unix, just include standard headers */
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <termios.h>
#include <unistd.h>
#include <signal.h>
#include <dirent.h>
#include <regex.h>

#endif /* PLATFORM_WINDOWS */

#endif /* TMUX_COMPAT_H */
