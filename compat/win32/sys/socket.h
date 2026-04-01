/*
 * sys/socket.h - Socket definitions wrapper for Windows
 *
 * Windows provides socket functionality through Winsock2.
 * This header provides POSIX-compatible definitions.
 */

#ifndef COMPAT_WIN32_SYS_SOCKET_H
#define COMPAT_WIN32_SYS_SOCKET_H

/* Winsock2 must be included before windows.h */
#ifndef _WINSOCK2API_
#include <winsock2.h>
#endif

#include <ws2tcpip.h>

/* Undefine macros that Windows might define which conflict with POSIX */
#ifdef msg_name
#undef msg_name
#endif
#ifdef msg_namelen
#undef msg_namelen
#endif
#ifdef msg_iov
#undef msg_iov
#endif
#ifdef msg_iovlen
#undef msg_iovlen
#endif
#ifdef msg_control
#undef msg_control
#endif
#ifdef msg_controllen
#undef msg_controllen
#endif
#ifdef msg_flags
#undef msg_flags
#endif

/* Undefine CMSG macros to prevent conflict with Windows definitions (e.g. CMSG_FIRSTHDR accessing .len) */
#ifdef CMSG_FIRSTHDR
#undef CMSG_FIRSTHDR
#endif
#ifdef CMSG_NXTHDR
#undef CMSG_NXTHDR
#endif
#ifdef CMSG_DATA
#undef CMSG_DATA
#endif
#ifdef CMSG_SPACE
#undef CMSG_SPACE
#endif
#ifdef CMSG_LEN
#undef CMSG_LEN
#endif
#ifdef CMSG_ALIGN
#undef CMSG_ALIGN
#endif


/* AF_UNIX support (Windows 10 1803+) */
#ifndef AF_UNIX
#define AF_UNIX 1
#endif

/* PF_* aliases */
#ifndef PF_UNIX
#define PF_UNIX   AF_UNIX
#define PF_LOCAL  AF_UNIX
#define PF_INET   AF_INET
#define PF_INET6  AF_INET6
#endif

/* Socket types (should be in winsock2.h, but ensure defined) */
#ifndef SOCK_STREAM
#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define SOCK_RAW    3
#endif

/* Socket options */
#ifndef SOL_SOCKET
#define SOL_SOCKET 0xffff
#endif

/* shutdown() how values */
#ifndef SHUT_RD
#define SHUT_RD   SD_RECEIVE
#define SHUT_WR   SD_SEND
#define SHUT_RDWR SD_BOTH
#endif

/* MSG flags */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0  /* Not applicable on Windows */
#endif

#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0  /* Use non-blocking sockets instead */
#endif

/*
 * msghdr structure for sendmsg/recvmsg compatibility
 * Note: Windows does not fully support SCM_RIGHTS (FD passing)
 * We guard these because ws2def.h might define them in newer SDKs
 */
#ifndef _STRUCT_MSGHDR_DEFINED
#define _STRUCT_MSGHDR_DEFINED
struct msghdr {
    void         *msg_name;
    socklen_t     msg_namelen;
    struct iovec *msg_iov;
    int           msg_iovlen;
    /* Union to handle ws2def.h Control macro conflict */
    union {
        void         *msg_control;
        void         *Control;
    };
    socklen_t     msg_controllen;
    int           msg_flags;
};
#endif

#ifndef _STRUCT_CMSGHDR_DEFINED
#define _STRUCT_CMSGHDR_DEFINED
#ifndef _WS2DEF_
struct cmsghdr {
    socklen_t cmsg_len;
    int       cmsg_level;
    int       cmsg_type;
    /* followed by unsigned char cmsg_data[] */
};
#endif
#endif

/* Control message macros */
#ifndef CMSG_ALIGN
#define CMSG_ALIGN(len) (((len) + sizeof(size_t) - 1) & ~(sizeof(size_t) - 1))
#endif

#ifndef CMSG_SPACE
#define CMSG_SPACE(len) (CMSG_ALIGN(sizeof(struct cmsghdr)) + CMSG_ALIGN(len))
#endif

#ifndef CMSG_LEN
#define CMSG_LEN(len) (CMSG_ALIGN(sizeof(struct cmsghdr)) + (len))
#endif

#ifndef CMSG_DATA
#define CMSG_DATA(cmsg) ((unsigned char *)((struct cmsghdr *)(cmsg) + 1))
#endif

#ifndef CMSG_FIRSTHDR
#define CMSG_FIRSTHDR(mhdr) \
    ((mhdr)->msg_controllen >= sizeof(struct cmsghdr) ? \
     (struct cmsghdr *)(mhdr)->msg_control : (struct cmsghdr *)NULL)
#endif

#ifndef CMSG_NXTHDR
#define CMSG_NXTHDR(mhdr, cmsg) \
    (((unsigned char *)(cmsg) + CMSG_ALIGN((cmsg)->cmsg_len) + \
      sizeof(struct cmsghdr)) > \
     ((unsigned char *)(mhdr)->msg_control + (mhdr)->msg_controllen) ? \
     (struct cmsghdr *)NULL : \
     (struct cmsghdr *)((unsigned char *)(cmsg) + CMSG_ALIGN((cmsg)->cmsg_len)))
#endif

/* SCM_RIGHTS - NOT SUPPORTED ON WINDOWS */
#ifndef SCM_RIGHTS
#define SCM_RIGHTS 1
#endif

/* iovec structure */
#ifndef _STRUCT_IOVEC_DEFINED
#define _STRUCT_IOVEC_DEFINED
struct iovec {
    void   *iov_base;
    size_t  iov_len;
};
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * sendmsg/recvmsg - stub implementations
 * Note: These do NOT support SCM_RIGHTS (file descriptor passing)
 */
extern SOCKET win32_get_real_socket(int fd);

static inline ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags) {
    WSABUF *bufs;
    DWORD sent = 0, i;
    int ret;
    SOCKET s = win32_get_real_socket(sockfd);

    if (msg->msg_iovlen <= 0)
        return 0;

    bufs = (WSABUF *)malloc(msg->msg_iovlen * sizeof(WSABUF));
    if (!bufs) {
        errno = ENOMEM;
        return -1;
    }

    for (i = 0; i < (DWORD)msg->msg_iovlen; i++) {
        bufs[i].buf = (char *)msg->msg_iov[i].iov_base;
        bufs[i].len = (ULONG)msg->msg_iov[i].iov_len;
    }

    ret = WSASend(s, bufs, msg->msg_iovlen, &sent, flags, NULL, NULL);
    free(bufs);

    if (ret == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) errno = EAGAIN;
        else if (err == WSAECONNRESET) errno = ECONNRESET;
        else if (err == WSAENOTCONN) errno = ENOTCONN;
        else if (err == WSAETIMEDOUT) errno = ETIMEDOUT;
        else errno = EIO;
        return -1;
    }

    return (ssize_t)sent;
}

static inline ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags) {
    WSABUF *bufs;
    DWORD received = 0, dwflags = flags, i;
    int ret;
    SOCKET s = win32_get_real_socket(sockfd);

    if (msg->msg_iovlen <= 0)
        return 0;

    bufs = (WSABUF *)malloc(msg->msg_iovlen * sizeof(WSABUF));
    if (!bufs) {
        errno = ENOMEM;
        return -1;
    }

    for (i = 0; i < (DWORD)msg->msg_iovlen; i++) {
        bufs[i].buf = (char *)msg->msg_iov[i].iov_base;
        bufs[i].len = (ULONG)msg->msg_iov[i].iov_len;
    }

    ret = WSARecv(s, bufs, msg->msg_iovlen, &received, &dwflags, NULL, NULL);
    free(bufs);

    if (ret == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) errno = EAGAIN;
        else if (err == WSAECONNRESET) errno = ECONNRESET;
        else if (err == WSAENOTCONN) errno = ENOTCONN;
        else if (err == WSAETIMEDOUT) errno = ETIMEDOUT;
        else errno = EIO;
        return -1;
    }

    msg->msg_flags = dwflags;
    msg->msg_controllen = 0;
    return (ssize_t)received;
}

#ifdef __cplusplus
}
#endif

#endif /* COMPAT_WIN32_SYS_SOCKET_H */
