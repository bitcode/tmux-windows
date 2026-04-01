#include "win32.h"
#include "term.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/*
 * win32-term.c - Stub implementation of terminfo functions for Windows
 *
 * Since we use VT passthrough (ConPTY), we don't need a real terminfo db.
 * We stub most functions, but tparm must work to expand parametrized caps.
 */

int setupterm(const char *term, int fildes, int *errret)
{
    (void)term;
    (void)fildes;
    if (errret) *errret = 1;
    return OK;
}

int tigetnum(const char *capname)
{
    (void)capname;
    return -1;
}

int tigetflag(const char *capname)
{
    (void)capname;
    return -1;
}

char *tigetstr(const char *capname)
{
    (void)capname;
    return (char *)NULL;
}

/*
 * tparm - expand a terminfo parameterized string
 *
 * Implements the subset of terminfo parameterized string operations that
 * tmux actually uses:
 *   %p1..%p9  push parameter N
 *   %i        increment params 1 and 2 by 1
 *   %d        pop and print as decimal integer
 *   %s        pop and print as string
 *   %+        pop two, add, push result (or %+%d: add literal constant)
 *   %-        pop two, subtract, push result
 *   %*        pop two, multiply, push result
 *   %>        if (tos > param), add param2 to tos
 *   %?%p1%t..%e..%;  conditional
 *   %{n}      push literal integer n
 *   %l        push length of string on stack
 *   %%        literal %
 *
 * Uses a static buffer — not thread-safe, matching POSIX tparm behavior.
 */
#define TPARM_BUF_SIZE  4096
#define TPARM_STACK_SIZE 32

char *
tparm(const char *str, ...)
{
    static char buf[TPARM_BUF_SIZE];
    va_list ap;
    /* Parameters: up to 9, stored as integers (string params treated as 0) */
    int params[9];
    int stack[TPARM_STACK_SIZE];
    int sp = 0;  /* stack pointer */
    const char *s = str;
    char *out = buf;
    char *end = buf + TPARM_BUF_SIZE - 1;
    int i;

    if (str == NULL || *str == '\0') {
        buf[0] = '\0';
        return buf;
    }

    /* Collect parameters */
    va_start(ap, str);
    for (i = 0; i < 9; i++)
        params[i] = va_arg(ap, int);
    va_end(ap);

    /* Scan for %i first to pre-increment params 0 and 1 */
    /* Actually %i is processed inline below */

#define PUSH(v) do { if (sp < TPARM_STACK_SIZE) stack[sp++] = (v); } while(0)
#define POP()   (sp > 0 ? stack[--sp] : 0L)

    while (*s && out < end) {
        if (*s != '%') {
            *out++ = *s++;
            continue;
        }
        s++;  /* skip '%' */
        switch (*s) {
        case '%':
            *out++ = '%';
            s++;
            break;
        case 'p':
            s++;
            if (*s >= '1' && *s <= '9') {
                PUSH(params[*s - '1']);
                s++;
            }
            break;
        case 'i':
            params[0]++;
            params[1]++;
            s++;
            break;
        case 'd': {
            int v = POP();
            int n = snprintf(out, (size_t)(end - out), "%d", v);
            if (n > 0) out += n;
            s++;
            break;
        }
        case 's': {
            /* String parameter — not commonly used, treat as empty */
            int v = POP();
            (void)v;
            s++;
            break;
        }
        case '{': {
            /* Literal integer: %{nnn} */
            long v = 0;
            s++;
            while (*s && *s != '}') {
                if (*s >= '0' && *s <= '9')
                    v = v * 10 + (*s - '0');
                s++;
            }
            if (*s == '}') s++;
            PUSH(v);
            break;
        }
        case 'l': {
            /* Push length of string on top of stack — not commonly used */
            s++;
            break;
        }
        case '+': {
            int b = POP(), a = POP();
            PUSH(a + b);
            s++;
            break;
        }
        case '-': {
            int b = POP(), a = POP();
            PUSH(a - b);
            s++;
            break;
        }
        case '*': {
            int b = POP(), a = POP();
            PUSH(a * b);
            s++;
            break;
        }
        case '/': {
            int b = POP(), a = POP();
            PUSH(b ? a / b : 0);
            s++;
            break;
        }
        case 'm': {
            int b = POP(), a = POP();
            PUSH(b ? a % b : 0);
            s++;
            break;
        }
        case 'A': {
            int b = POP(), a = POP();
            PUSH(a && b);
            s++;
            break;
        }
        case 'O': {
            int b = POP(), a = POP();
            PUSH(a || b);
            s++;
            break;
        }
        case '!': {
            int a = POP();
            PUSH(!a);
            s++;
            break;
        }
        case '~': {
            int a = POP();
            PUSH(~a);
            s++;
            break;
        }
        case '=': {
            int b = POP(), a = POP();
            PUSH(a == b);
            s++;
            break;
        }
        case '>': {
            int b = POP(), a = POP();
            PUSH(a > b);
            s++;
            break;
        }
        case '<': {
            int b = POP(), a = POP();
            PUSH(a < b);
            s++;
            break;
        }
        case '?':
            /* Start of conditional: %?<cond>%t<then>%e<else>%; */
            s++;
            break;
        case 't': {
            /* Then branch: if TOS is false, skip to %e or %; */
            int cond = POP();
            s++;
            if (!cond) {
                /* Skip to matching %e or %; */
                int depth = 1;
                while (*s && depth > 0) {
                    if (*s == '%') {
                        s++;
                        if (*s == '?') depth++;
                        else if (*s == ';') { depth--; if (depth == 0) { s++; break; } }
                        else if (*s == 'e' && depth == 1) { s++; break; }
                        if (*s) s++;
                    } else {
                        s++;
                    }
                }
            }
            break;
        }
        case 'e': {
            /* Else branch: skip to %; */
            s++;
            int depth = 1;
            while (*s && depth > 0) {
                if (*s == '%') {
                    s++;
                    if (*s == '?') depth++;
                    else if (*s == ';') { depth--; if (depth == 0) { s++; break; } }
                    if (*s) s++;
                } else {
                    s++;
                }
            }
            break;
        }
        case ';':
            /* End of conditional */
            s++;
            break;
        case 'c': {
            /* Pop as character */
            int v = POP();
            if (out < end) *out++ = (char)v;
            s++;
            break;
        }
        default:
            /* Unknown sequence — pass through literally */
            if (out < end) *out++ = '%';
            if (out < end) *out++ = *s;
            s++;
            break;
        }
    }

    *out = '\0';
    return buf;

#undef PUSH
#undef POP
}

int tputs(const char *str, int affcnt, int (*putc_fn)(int))
{
    (void)affcnt;
    if (str == NULL || putc_fn == NULL)
        return ERR;
    while (*str)
        putc_fn((unsigned char)*str++);
    return OK;
}

int del_curterm(TERMINAL *oterm) {
	(void)oterm;
	return (0);
}
