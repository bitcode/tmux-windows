/*
 * sys/uio.h - Scatter/gather I/O definitions for Windows
 */

#ifndef COMPAT_WIN32_SYS_UIO_H
#define COMPAT_WIN32_SYS_UIO_H

#include <stddef.h>
#include <winsock2.h>

/* iovec structure for scatter/gather I/O */
#ifndef _STRUCT_IOVEC_DEFINED
#define _STRUCT_IOVEC_DEFINED
struct iovec {
    void   *iov_base;  /* Base address */
    size_t  iov_len;   /* Length */
};
#endif

/* Maximum number of iovec entries */
#ifndef IOV_MAX
#define IOV_MAX 1024
#endif

#ifndef UIO_MAXIOV
#define UIO_MAXIOV IOV_MAX
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * readv/writev - scatter/gather I/O
 * Implemented in win32-io.c
 */
ssize_t readv(int fd, const struct iovec *iov, int iovcnt);
ssize_t writev(int fd, const struct iovec *iov, int iovcnt);

#ifdef __cplusplus
}
#endif

#endif /* COMPAT_WIN32_SYS_UIO_H */
