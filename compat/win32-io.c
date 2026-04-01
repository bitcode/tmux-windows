/*
 * win32-io.c - I/O helpers for tmux Windows port
 *
 * Provides POSIX I/O compatibility functions.
 */

#include "win32.h"
#include "win32/sys/uio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <term.h>

TERMINAL *cur_term = NULL;

/*
 * readv - scatter read
 */
ssize_t
readv(int fd, const struct iovec *iov, int iovcnt)
{
    ssize_t total = 0;
    int i;

    for (i = 0; i < iovcnt; i++) {
        ssize_t n = win32_read(fd, iov[i].iov_base, (unsigned int)iov[i].iov_len);
        if (n < 0) {
            if (total > 0)
                return total;
            return -1;
        }
        total += n;
        if ((size_t)n < iov[i].iov_len)
            break;
    }

    return total;
}

/*
 * writev - gather write
 */
ssize_t
writev(int fd, const struct iovec *iov, int iovcnt)
{
    ssize_t total = 0;
    int i;

    for (i = 0; i < iovcnt; i++) {
        ssize_t n = win32_write(fd, iov[i].iov_base, (unsigned int)iov[i].iov_len);
        if (n < 0) {
            if (total > 0)
                return total;
            return -1;
        }
        total += n;
        if ((size_t)n < iov[i].iov_len)
            break;
    }

    return total;
}

/*
 * Terminal I/O functions
 *
 * These are stubs that interface with Windows Console API.
 * Full implementation requires ConPTY integration.
 */

/*
 * tcgetattr - get terminal attributes
 */
int
win32_tcgetattr(int fd, struct termios *termios_p)
{
    HANDLE h;
    DWORD mode;

    if (termios_p == NULL) {
        errno = EINVAL;
        return -1;
    }

    h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }

    memset(termios_p, 0, sizeof(*termios_p));

    /* Try to get console mode */
    if (GetConsoleMode(h, &mode)) {
        /* Convert Windows console mode to termios flags */
        if (mode & ENABLE_ECHO_INPUT)
            termios_p->c_lflag |= ECHO;
        if (mode & ENABLE_LINE_INPUT)
            termios_p->c_lflag |= ICANON;
        if (mode & ENABLE_PROCESSED_INPUT)
            termios_p->c_lflag |= ISIG;
        if (mode & ENABLE_VIRTUAL_TERMINAL_INPUT)
            termios_p->c_iflag |= IUTF8;
    }

    /* Set reasonable defaults */
    termios_p->c_cc[VMIN] = 1;
    termios_p->c_cc[VTIME] = 0;
    termios_p->c_cflag = CS8 | CREAD;

    return 0;
}

/*
 * tcsetattr - set terminal attributes
 */
int
win32_tcsetattr(int fd, int optional_actions, const struct termios *termios_p)
{
    HANDLE h;
    DWORD mode = 0;

    (void)optional_actions;

    if (termios_p == NULL) {
        errno = EINVAL;
        return -1;
    }

    h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }

    /* Convert termios flags to Windows console mode */
    if (termios_p->c_lflag & ECHO)
        mode |= ENABLE_ECHO_INPUT;
    if (termios_p->c_lflag & ICANON)
        mode |= ENABLE_LINE_INPUT;
    if (termios_p->c_lflag & ISIG)
        mode |= ENABLE_PROCESSED_INPUT;

    /* Always enable virtual terminal processing for modern terminals */
    mode |= ENABLE_VIRTUAL_TERMINAL_INPUT;

    if (!SetConsoleMode(h, mode)) {
        /* Not a console - ignore */
        return 0;
    }

    return 0;
}

/*
 * cfmakeraw - set raw mode
 */
void
win32_cfmakeraw(struct termios *termios_p)
{
    if (termios_p == NULL)
        return;

    termios_p->c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP |
                           INLCR | IGNCR | ICRNL | IXON);
    termios_p->c_oflag &= ~OPOST;
    termios_p->c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    termios_p->c_cflag &= ~(CSIZE | PARENB);
    termios_p->c_cflag |= CS8;
}

/*
 * Baud rate functions (not really applicable on Windows)
 */
speed_t
win32_cfgetispeed(const struct termios *termios_p)
{
    return termios_p ? termios_p->c_ispeed : B38400;
}

speed_t
win32_cfgetospeed(const struct termios *termios_p)
{
    return termios_p ? termios_p->c_ospeed : B38400;
}

int
win32_cfsetispeed(struct termios *termios_p, speed_t speed)
{
    if (termios_p)
        termios_p->c_ispeed = speed;
    return 0;
}

int
win32_cfsetospeed(struct termios *termios_p, speed_t speed)
{
    if (termios_p)
        termios_p->c_ospeed = speed;
    return 0;
}

/*
 * ioctl - I/O control
 */
int
win32_ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    void *arg;
    HANDLE h;

    va_start(ap, request);
    arg = va_arg(ap, void *);
    va_end(ap);

    /* Socket fds (>= WINSOCK_FD_OFFSET) are not CRT fds — _get_osfhandle
     * would assert on them. Handle terminal ioctls via the PTY map. */
    if (win32_from_map(fd) != INVALID_SOCKET) {
        switch (request) {
        case TIOCGWINSZ: {
            struct winsize *ws = arg;
            win32_pty_t *pty = win32_pty_lookup(fd);
            int cols, rows;
            win32_pty_get_size(pty, &cols, &rows);
            ws->ws_col = (unsigned short)cols;
            ws->ws_row = (unsigned short)rows;
            ws->ws_xpixel = ws->ws_ypixel = 0;
            return 0;
        }
        case TIOCSWINSZ: {
            struct winsize *ws = arg;
            win32_pty_t *pty = win32_pty_lookup(fd);
            if (pty != NULL && ws->ws_col > 0 && ws->ws_row > 0) {
                win32_log("win32_ioctl TIOCSWINSZ: fd=%d cols=%d rows=%d\n",
                          fd, ws->ws_col, ws->ws_row);
                win32_pty_resize(pty, ws->ws_col, ws->ws_row);
            }
            return 0;
        }
        default:
            errno = EBADF;
            return -1;
        }
    }

    h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }

    switch (request) {
    case TIOCGWINSZ:
        {
            struct winsize *ws = arg;
            CONSOLE_SCREEN_BUFFER_INFO csbi;

            if (GetConsoleScreenBufferInfo(h, &csbi)) {
                ws->ws_col = csbi.srWindow.Right - csbi.srWindow.Left + 1;
                ws->ws_row = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
                ws->ws_xpixel = 0;
                ws->ws_ypixel = 0;
                return 0;
            }

            /* Not a console, return default size */
            ws->ws_col = 80;
            ws->ws_row = 24;
            ws->ws_xpixel = 0;
            ws->ws_ypixel = 0;
            return 0;
        }

    case TIOCSWINSZ:
        {
            /* Setting window size not directly supported for console */
            /* This would be handled by ConPTY's ResizePseudoConsole */
            return 0;
        }

    case FIONREAD:
        {
            DWORD available = 0;
            u_long *bytes = arg;

            /* Try as pipe */
            if (PeekNamedPipe(h, NULL, 0, NULL, &available, NULL)) {
                *bytes = available;
                return 0;
            }

            /* Try as socket */
            if (ioctlsocket((SOCKET)(intptr_t)h, FIONREAD, bytes) == 0)
                return 0;

            /* Try as console */
            {
                INPUT_RECORD ir[1];
                DWORD count;
                if (PeekConsoleInputW(h, ir, 1, &count)) {
                    *bytes = count > 0 ? 1 : 0;
                    return 0;
                }
            }

            *bytes = 0;
            return 0;
        }

    case FIONBIO:
        {
            u_long *mode = arg;
            if (ioctlsocket((SOCKET)(intptr_t)h, FIONBIO, mode) == 0)
                return 0;
            /* Not a socket - ignore */
            return 0;
        }

    default:
        errno = EINVAL;
        return -1;
    }
}

/*
 * isatty - check if fd is a terminal
 */
int
win32_isatty(int fd)
{
    HANDLE h;
    DWORD mode;

    h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;

    /* Check if it's a console */
    if (GetConsoleMode(h, &mode))
        return 1;

    /* Check file type */
    if (GetFileType(h) == FILE_TYPE_CHAR)
        return 1;

    return 0;
}

/*
 * ttyname - get terminal name
 */
char *
win32_ttyname(int fd)
{
    static char name[32];

    if (!win32_isatty(fd))
        return NULL;

    snprintf(name, sizeof(name), "CON");
    return name;
}
