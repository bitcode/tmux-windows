#ifndef _SYS_FILE_H
#define _SYS_FILE_H

#include "win32.h"

#define LOCK_SH 0x01
#define LOCK_EX 0x02
#define LOCK_NB 0x04
#define LOCK_UN 0x08

/* flock is emulated as a no-op in win32.h, but we provide declaration here */

#endif
