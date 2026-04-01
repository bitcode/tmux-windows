
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <arpa/inet.h>
#include <limits.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "compat.h"

#undef msg_control

#include "imsg.h"

#ifdef msg_control
#error "msg_control IS DEFINED"
#else
#error "msg_control IS NOT DEFINED"
#endif

void stub() {}
