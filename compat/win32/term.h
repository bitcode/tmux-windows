#ifndef _TERM_H
#define _TERM_H

#include "curses.h"

#define OK (0)
#define ERR (-1)

typedef struct {
    int _dummy;
} TERMINAL;

#ifdef __cplusplus
extern "C" {
#endif

extern TERMINAL *cur_term;

int setupterm(const char *, int, int *);
int tigetnum(const char *);
int tigetflag(const char *);
char *tigetstr(const char *);
char *tparm(const char *, ...);
int tputs(const char *, int, int (*)(int));
int del_curterm(TERMINAL *);

int tgetent(char *, const char *);
int tgetnum(const char *);
int tgetflag(const char *);
char *tgetstr(const char *, char **);
char *tgoto(const char *, int, int);

#ifdef __cplusplus
}
#endif

#endif
