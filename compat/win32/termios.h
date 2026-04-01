/*
 * termios.h - Terminal I/O definitions for Windows
 *
 * Windows uses Console API instead of termios. This header provides
 * compatibility structures and function declarations that map to
 * Windows Console and ConPTY APIs.
 */

#ifndef COMPAT_WIN32_TERMIOS_H
#define COMPAT_WIN32_TERMIOS_H

#include "win32.h"

/*
 * All termios structures and constants are defined in win32.h
 * This header exists for source compatibility with code that
 * includes <termios.h>
 */

#endif /* COMPAT_WIN32_TERMIOS_H */
