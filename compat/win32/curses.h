#ifndef _CURSES_H
#define _CURSES_H

#include "win32.h"

#define OK (0)
#define ERR (-1)

/* Dummy types and macros for tmux */
typedef void * WINDOW;
typedef unsigned long chtype;

#define COLOR_PAIR(n) (n)
#define A_NORMAL 0
#define A_REVERSE 0x0001
#define A_BOLD 0x0002
#define A_UNDERLINE 0x0004

#endif
