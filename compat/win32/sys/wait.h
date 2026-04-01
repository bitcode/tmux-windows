/*
 * sys/wait.h - POSIX wait definitions for Windows
 */

#ifndef COMPAT_WIN32_SYS_WAIT_H
#define COMPAT_WIN32_SYS_WAIT_H

#include "../win32.h"

/* Wait options */
#ifndef WNOHANG
#define WNOHANG   1
#define WUNTRACED 2
#define WCONTINUED 8
#endif

/* These macros are defined in win32.h, but ensure they exist */
#ifndef WIFEXITED
#define WIFEXITED(status)   (((status) & 0x7f) == 0)
#define WEXITSTATUS(status) (((status) >> 8) & 0xff)
#define WIFSIGNALED(status) (((status) & 0x7f) != 0 && ((status) & 0x7f) != 0x7f)
#define WTERMSIG(status)    ((status) & 0x7f)
#define WIFSTOPPED(status)  (((status) & 0xff) == 0x7f)
#define WSTOPSIG(status)    (((status) >> 8) & 0xff)
#define WCOREDUMP(status)   ((status) & 0x80)
#endif

#ifndef WAIT_ANY
#define WAIT_ANY  (-1)
#define WAIT_MYPGRP 0
#endif

#endif /* COMPAT_WIN32_SYS_WAIT_H */
