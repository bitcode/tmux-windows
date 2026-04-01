/*
 * sys/ioctl.h - ioctl definitions for Windows
 *
 * On Windows, terminal control is done via Console API and ConPTY,
 * not ioctl. This header provides stub definitions for compatibility.
 */

#ifndef COMPAT_WIN32_SYS_IOCTL_H
#define COMPAT_WIN32_SYS_IOCTL_H

#include "../win32.h"

/* Terminal ioctl requests */
#ifndef TIOCGWINSZ
#define TIOCGWINSZ  0x5413  /* Get window size */
#define TIOCSWINSZ  0x5414  /* Set window size */
#define TIOCGPGRP   0x540F  /* Get foreground process group */
#define TIOCSPGRP   0x5410  /* Set foreground process group */
#define TIOCSCTTY   0x540E  /* Set controlling terminal */
#define TIOCNOTTY   0x5422  /* Give up controlling terminal */
#define TIOCSTI     0x5412  /* Simulate terminal input */
#define TIOCOUTQ    0x5411  /* Output queue size */
#define TIOCINQ     0x541B  /* Input queue size */
#define FIONREAD    TIOCINQ
#define FIONBIO     0x5421  /* Set non-blocking I/O */
#define FIOASYNC    0x5452  /* Set async I/O */
#endif

/* winsize structure is defined in win32.h */

#endif /* COMPAT_WIN32_SYS_IOCTL_H */
