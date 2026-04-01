#ifndef _COMPAT_WIN32_PATHS_H_
#define _COMPAT_WIN32_PATHS_H_
#include "../win32.h"

/* Windows-specific path definitions */
#ifndef _PATH_BSHELL
#define _PATH_BSHELL "cmd.exe"
#endif

#ifndef _PATH_TMP
/* Use Windows TEMP environment variable at runtime, fallback to C:\Windows\Temp */
#define _PATH_TMP "C:\\Windows\\Temp\\"
#endif

#ifndef _PATH_DEVNULL
#define _PATH_DEVNULL "NUL"
#endif

#ifndef _PATH_DEFPATH
#define _PATH_DEFPATH "C:\\Windows\\System32;C:\\Windows"
#endif

#endif
