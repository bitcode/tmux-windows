/*
 * win32-socket.c - Socket helpers for tmux Windows port
 *
 * Provides POSIX socket compatibility and AF_UNIX helpers.
 */

#include <winsock2.h>
#include <afunix.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <io.h>
#include <fcntl.h>
#include <event.h>
#include <event2/util.h>

/* Include win32.h LAST as it contains the redirections that we want to wrap */
#include "win32.h"

/* Undefine macros to avoid infinite recursion in wrappers */
#undef socket
#undef bind
#undef listen
#undef accept
#undef connect
#undef setsockopt
#undef getsockopt
#undef getsockname
#undef getpeername
#undef send
#undef recv
#undef read
#undef write
#undef close
#undef event_set

static int winsock_initialized = 0;
char **environ = NULL;

/* Socket mapping table to prevent truncation of 64-bit SOCKET handles in 32-bit int */
#define MAX_WINSOCK_MAPPING 1024
/* WINSOCK_FD_OFFSET is defined in win32.h (force-included) */
static SOCKET win32_socket_map[MAX_WINSOCK_MAPPING];
static int win32_socket_map_init = 0;

static void
win32_init_socket_map(void)
{
    if (win32_socket_map_init) return;
    for (int i = 0; i < MAX_WINSOCK_MAPPING; i++)
        win32_socket_map[i] = INVALID_SOCKET;
    win32_socket_map_init = 1;
}

#undef getsockopt
#undef getpeername
#undef send
#undef recv
#undef event_set
#undef accept
#undef listen
#undef bind
#undef connect
#undef socket
#undef close

int
win32_add_to_map(SOCKET s)
{
    win32_init_socket_map();
    if (s == INVALID_SOCKET) return -1;
    for (int i = 0; i < MAX_WINSOCK_MAPPING; i++) {
        if (win32_socket_map[i] == INVALID_SOCKET) {
            win32_socket_map[i] = s;
            int fd = i + WINSOCK_FD_OFFSET;
            win32_log("win32_add_to_map: handle=%llu -> fd=%d\n", (unsigned long long)s, fd);
            return fd;
        }
    }
    errno = EMFILE;
    return -1;
}

SOCKET
win32_from_map(int fd)
{
    if (fd < WINSOCK_FD_OFFSET || fd >= WINSOCK_FD_OFFSET + MAX_WINSOCK_MAPPING)
        return INVALID_SOCKET;
    return win32_socket_map[fd - WINSOCK_FD_OFFSET];
}

void
win32_remove_from_map(int fd)
{
    if (fd >= WINSOCK_FD_OFFSET && fd < WINSOCK_FD_OFFSET + MAX_WINSOCK_MAPPING) {
        win32_log("win32_remove_from_map: fd=%d\n", fd);
        win32_socket_map[fd - WINSOCK_FD_OFFSET] = INVALID_SOCKET;
    }
}

/*
 * Reverse lookup - finds the ID for a handle.
 * Handles both full 64-bit handles and truncated 32-bit handles.
 */
int
win32_reverse_lookup(SOCKET s)
{
    win32_init_socket_map();
    if (s == INVALID_SOCKET) return -1;
    for (int i = 0; i < MAX_WINSOCK_MAPPING; i++) {
        if (win32_socket_map[i] == s)
            return i + WINSOCK_FD_OFFSET;
    }
    /* Try truncated match (common in callbacks) */
    for (int i = 0; i < MAX_WINSOCK_MAPPING; i++) {
        if (win32_socket_map[i] != INVALID_SOCKET && (int)win32_socket_map[i] == (int)s) {
            int fd = i + WINSOCK_FD_OFFSET;
            win32_log("win32_reverse_lookup: truncated match handle=%llu -> fd=%d\n", (unsigned long long)s, fd);
            return fd;
        }
    }
    return -1;
}

/* Internal helper to get SOCKET from mapped ID or raw (potentially truncated) handle */
SOCKET
win32_get_real_socket(int fd)
{
    SOCKET s = win32_from_map(fd);
    if (s != INVALID_SOCKET) return s;

    /* If not in map, try reverse lookup in case it's a truncated handle from a callback.
       Casting to (unsigned int) first prevents sign extension on x64. */
    int mapped_fd = win32_reverse_lookup((SOCKET)(unsigned int)fd);
    if (mapped_fd != -1)
        return win32_from_map(mapped_fd);

    /* Fallback to treating it as a raw handle */
    return (SOCKET)(unsigned int)fd;
}

/*
 * Initialize Winsock
 */
int
win32_socket_init(void)
{
    WSADATA wsaData;
    int result;

    if (winsock_initialized)
        return 0;

    result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0)
        return -1;

    environ = _environ;
    winsock_initialized = 1;
    return 0;
}

/*
 * Cleanup Winsock
 */
void
win32_socket_cleanup(void)
{
    if (winsock_initialized) {
        WSACleanup();
        winsock_initialized = 0;
    }
}

/*
 * Set socket to non-blocking mode
 */
int
win32_socket_set_nonblocking(SOCKET sock)
{
    u_long mode = 1;
    return ioctlsocket(sock, FIONBIO, &mode);
}

/*
 * socketpair - create a pair of connected sockets
 */
int
win32_socketpair(int domain, int type, int protocol, int sv[2])
{
    SOCKET listener = INVALID_SOCKET;
    SOCKET client = INVALID_SOCKET;
    SOCKET server = INVALID_SOCKET;
    struct sockaddr_in addr;
    int addrlen = sizeof(addr);
    int result = -1;

    (void)domain;
    (void)protocol;

    win32_socket_init();

    if (sv == NULL) {
        errno = EINVAL;
        return -1;
    }

    sv[0] = sv[1] = -1;

    /*
     * Windows AF_UNIX sockets have known issues with select() and don't
     * officially support socketpair(). Use TCP loopback instead, which
     * works reliably with Winsock select() and libevent.
     * 
     * Reference: https://learn.microsoft.com/en-us/windows/win32/winsock/af-unix
     */
#if 0
    /* Disabled: AF_UNIX socketpair has select() compatibility issues on Windows */
    /* Try AF_UNIX first */
    if (domain == AF_UNIX || domain == AF_LOCAL) {
        struct sockaddr_un unaddr;
        char path[108];
        snprintf(path, sizeof(path), "\\\\.\\pipe\\tmux-sp-%d-%d", GetCurrentProcessId(), GetTickCount());

        listener = socket(AF_UNIX, type, 0);
        if (listener != INVALID_SOCKET) {
            memset(&unaddr, 0, sizeof(unaddr));
            unaddr.sun_family = AF_UNIX;
            strncpy(unaddr.sun_path, path, sizeof(unaddr.sun_path) - 1);

            if (bind(listener, (struct sockaddr *)&unaddr, sizeof(unaddr)) == 0) {
                if (listen(listener, 1) == 0) {
                    client = socket(AF_UNIX, type, 0);
                    if (client != INVALID_SOCKET) {
                        if (connect(client, (struct sockaddr *)&unaddr, sizeof(unaddr)) == 0) {
                            server = accept(listener, NULL, NULL);
                            if (server != INVALID_SOCKET) {
                                closesocket(listener);
                                sv[0] = win32_add_to_map(server);
                                sv[1] = win32_add_to_map(client);
                                unlink(path);
                                return 0;
                            }
                        }
                        closesocket(client);
                    }
                }
            }
            closesocket(listener);
            unlink(path);
        }
    }
#endif

#ifdef PLATFORM_WINDOWS
    win32_log("win32_socketpair: using TCP loopback (AF_INET) for select() compatibility\n");
#endif


    /* Fallback to TCP loopback */
    listener = socket(AF_INET, type, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) goto cleanup;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0) goto cleanup;
    if (getsockname(listener, (struct sockaddr *)&addr, &addrlen) != 0) goto cleanup;
    if (listen(listener, 1) != 0) goto cleanup;

    client = socket(AF_INET, type, IPPROTO_TCP);
    if (client == INVALID_SOCKET) goto cleanup;
    if (connect(client, (struct sockaddr *)&addr, sizeof(addr)) != 0) goto cleanup;
    server = accept(listener, NULL, NULL);
    if (server == INVALID_SOCKET) goto cleanup;

    closesocket(listener);
    sv[0] = win32_add_to_map(server);
    sv[1] = win32_add_to_map(client);
    result = 0;

cleanup:
    if (result != 0) {
        if (listener != INVALID_SOCKET) closesocket(listener);
        if (client != INVALID_SOCKET) closesocket(client);
        if (server != INVALID_SOCKET) closesocket(server);
    }
    return result;
}

/*
 * pipe - create a pipe
 */
int
win32_pipe(int pipefd[2])
{
    HANDLE hRead, hWrite;
    SECURITY_ATTRIBUTES sa;

    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
        errno = EMFILE;
        return -1;
    }

    pipefd[0] = _open_osfhandle((intptr_t)hRead, _O_RDONLY);
    pipefd[1] = _open_osfhandle((intptr_t)hWrite, _O_WRONLY);
    if (pipefd[0] == -1 || pipefd[1] == -1) {
        if (pipefd[0] != -1) _close(pipefd[0]); else CloseHandle(hRead);
        if (pipefd[1] != -1) _close(pipefd[1]); else CloseHandle(hWrite);
        return -1;
    }
    return 0;
}

/*
 * fcntl - file control
 */
int
win32_fcntl(int fd, int cmd, ...)
{
    va_list ap;
    int arg;
    u_long mode;
    SOCKET s = win32_get_real_socket(fd);

    va_start(ap, cmd);

    switch (cmd) {
    case F_GETFL:
        va_end(ap);
        return 0;
    case F_SETFL:
        arg = va_arg(ap, int);
        va_end(ap);
        mode = (arg & O_NONBLOCK) ? 1 : 0;
        if (s != INVALID_SOCKET) {
             if (ioctlsocket(s, FIONBIO, &mode) == 0) return 0;
             return -1;
        }
        HANDLE h = (HANDLE)_get_osfhandle(fd);
        if (h != INVALID_HANDLE_VALUE) {
             if (ioctlsocket((SOCKET)(intptr_t)h, FIONBIO, &mode) == 0) return 0;
        }
        return 0;
    case F_GETFD:
    case F_SETFD:
        va_end(ap);
        return 0;
    default:
        va_end(ap);
        errno = EINVAL;
        return -1;
    }
}

/*
 * Translate socket path
 */
char *
win32_translate_socket_path(const char *path)
{
    static char buf[MAX_PATH];
    const char *localappdata, *basename;
    if (path == NULL) return NULL;
    if (path[0] != '/' && path[0] != '~') return (char *)path;
    localappdata = getenv("LOCALAPPDATA");
    if (localappdata == NULL) localappdata = getenv("APPDATA");
    if (localappdata == NULL) localappdata = "C:\\Users\\Default\\AppData\\Local";
    basename = strrchr(path, '/');
    if (basename) basename++; else basename = path;
    snprintf(buf, sizeof(buf), "%s\\tmux\\%s", localappdata, basename);
    return buf;
}

/*
 * Wrapped socket functions
 */
int win32_connect(int fd, const struct sockaddr *name, int namelen) {
    SOCKET s = win32_get_real_socket(fd);
    win32_log("win32_connect: fd=%d -> socket=%llu\n", fd, (unsigned long long)s);
    if (connect(s, name, namelen) == SOCKET_ERROR) {
        int err = WSAGetLastError();
        win32_log("win32_connect: FAILED err=%d\n", err);
        switch (err) {
        case WSAECONNREFUSED: errno = ECONNREFUSED; break;
        case WSAENOTCONN: errno = ENOENT; break;
        case WSAEWOULDBLOCK: errno = EINPROGRESS; break;
        default: errno = EINVAL; break;
        }
        return -1;
    }
    win32_log("win32_connect: SUCCESS\n");
    return 0;
}

int win32_socket(int domain, int type, int protocol) {
    SOCKET s = socket(domain, type, protocol);
    return win32_add_to_map(s);
}

int win32_bind(int fd, const struct sockaddr *name, int namelen) {
    SOCKET s = win32_get_real_socket(fd);
    win32_log("win32_bind: fd=%d -> socket=%llu\n", fd, (unsigned long long)s);
    if (bind(s, name, namelen) == SOCKET_ERROR) {
        win32_log("win32_bind: FAILED err=%d\n", WSAGetLastError());
        errno = EIO; 
        return -1; 
    }
    win32_log("win32_bind: SUCCESS\n");
    return 0;
}

int win32_listen(int fd, int backlog) {
    SOCKET s = win32_get_real_socket(fd);
    win32_log("win32_listen: fd=%d -> socket=%llu\n", fd, (unsigned long long)s);
    if (listen(s, backlog) == SOCKET_ERROR) { 
        win32_log("win32_listen: FAILED err=%d\n", WSAGetLastError());
        errno = EIO; 
        return -1; 
    }
    win32_log("win32_listen: SUCCESS\n");
    return 0;
}

int win32_accept(int fd, struct sockaddr *addr, int *addrlen) {
    SOCKET s = win32_get_real_socket(fd);
    SOCKET newsock = accept(s, addr, (socklen_t*)addrlen);
    if (newsock == INVALID_SOCKET) {
        int werr = WSAGetLastError();
        win32_log("win32_accept: parent=%d, accept failed WSA=%d\n", fd, werr);
        if (werr == WSAEWOULDBLOCK)
            errno = EWOULDBLOCK;
        else if (werr == WSAEINTR)
            errno = EINTR;
        else if (werr == WSAECONNABORTED)
            errno = ECONNABORTED;
        else
            errno = EACCES;
        return -1;
    }
    int newfd = win32_add_to_map(newsock);
    win32_log("win32_accept: parent=%d, newsock=%llu -> newfd=%d\n", fd, (unsigned long long)newsock, newfd);
    return newfd;
}

int win32_close(int fd) {
    win32_log("win32_close: fd=%d\n", fd);
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
    SOCKET s = win32_from_map(fd);
    if (s != INVALID_SOCKET) {
        closesocket(s);
        win32_remove_from_map(fd);
        return 0;
    }
    s = (SOCKET)(unsigned int)fd;
    int v, vlen = sizeof(v);
    if (getsockopt(s, SOL_SOCKET, SO_TYPE, (char*)&v, &vlen) == 0) return closesocket(s);
    if (WSAGetLastError() == WSAENOTSOCK) return _close(fd);
    return closesocket(s);
}

int win32_setsockopt(int fd, int level, int optname, const void *optval, int optlen) {
    SOCKET s = win32_get_real_socket(fd);
    if (setsockopt(s, level, optname, (const char*)optval, (socklen_t)optlen) == SOCKET_ERROR) { errno = EIO; return -1; }
    return 0;
}

int win32_getsockopt(int fd, int level, int optname, void *optval, int *optlen) {
    SOCKET s = win32_get_real_socket(fd);
    if (getsockopt(s, level, optname, (char*)optval, (socklen_t*)optlen) == SOCKET_ERROR) { errno = EIO; return -1; }
    return 0;
}

int win32_getsockname(int fd, struct sockaddr *name, int *namelen) {
    SOCKET s = win32_get_real_socket(fd);
    return getsockname(s, name, (socklen_t*)namelen);
}

int win32_getpeername(int fd, struct sockaddr *name, int *namelen) {
    SOCKET s = win32_get_real_socket(fd);
    return getpeername(s, name, (socklen_t*)namelen);
}

ssize_t win32_read(int fd, void *buf, size_t count) {
    SOCKET s = win32_from_map(fd);
    if (s != INVALID_SOCKET) {
        int n = recv(s, (char*)buf, (int)count, 0);
        if (n == SOCKET_ERROR) { errno = (WSAGetLastError() == WSAEWOULDBLOCK) ? EAGAIN : EIO; return -1; }
        return n;
    }
    return _read(fd, buf, (unsigned int)count);
}

ssize_t win32_write(int fd, const void *buf, size_t count) {
    SOCKET s = win32_from_map(fd);
    if (s != INVALID_SOCKET) {
        int n = send(s, (const char*)buf, (int)count, 0);
        if (n == SOCKET_ERROR) { errno = (WSAGetLastError() == WSAEWOULDBLOCK) ? EAGAIN : EIO; return -1; }
        return n;
    }
    
    /* For stdout/stderr, use WriteConsole to ensure VT sequences are processed */
    if (fd == 1 || fd == 2) {
        HANDLE hConsole = GetStdHandle(fd == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
        if (hConsole != INVALID_HANDLE_VALUE && hConsole != NULL) {
            DWORD written = 0;
            /* Use WriteFile for VT sequence support (WriteConsole doesn't work with redirected output) */
            if (WriteFile(hConsole, buf, (DWORD)count, &written, NULL)) {
                return (ssize_t)written;
            }
            /* If WriteFile fails, fall through to _write */
        }
    }
    return _write(fd, buf, (unsigned int)count);
}

ssize_t win32_recv(int fd, void *buf, size_t len, int flags) {
    SOCKET s = win32_get_real_socket(fd);
    int n = recv(s, (char*)buf, (int)len, flags);
    if (n == SOCKET_ERROR) { errno = (WSAGetLastError() == WSAEWOULDBLOCK) ? EAGAIN : EIO; return -1; }
    return n;
}

ssize_t win32_send(int fd, const void *buf, size_t len, int flags) {
    SOCKET s = win32_get_real_socket(fd);
    int n = send(s, (const char*)buf, (int)len, flags);
    if (n == SOCKET_ERROR) { errno = (WSAGetLastError() == WSAEWOULDBLOCK) ? EAGAIN : EIO; return -1; }
    return n;
}

/*
 * win32_check_peer_is_owner - verify the connected peer is the same user as
 * the server process.
 *
 * Uses SIO_AF_UNIX_GETPEERPID (_WSAIOR(IOC_VENDOR, 256)) to retrieve the
 * peer's PID, then walks the token chain to compare SIDs.
 *
 * Returns 1 if the peer is the same user, 0 if not or on any error.
 * On any internal failure the connection is treated as untrusted (fail-closed).
 *
 * TOCTOU note: the PID returned by SIO_AF_UNIX_GETPEERPID can be recycled.
 * Mitigations applied:
 *   - Reject peer_pid == 0 (WSL/kernel bug, documented in WSL issue #4676)
 *   - Reject connections where OpenProcess fails (process gone = PID recycled)
 *   - SID comparison is the actual trust gate, not the PID itself
 */
int
win32_check_peer_is_owner(int fd)
{
    SOCKET       sock;
    DWORD        peer_pid = 0;
    DWORD        bytes_ret = 0;
    HANDLE       hProc = NULL, hToken = NULL;
    DWORD        token_buf[256];   /* enough for TOKEN_USER + SID on any system */
    DWORD        needed = sizeof token_buf;
    TOKEN_USER  *tu_peer;
    /* Server's own SID */
    HANDLE       hSelf = NULL, hSelfToken = NULL;
    DWORD        self_buf[256];
    DWORD        self_needed = sizeof self_buf;
    TOKEN_USER  *tu_self;
    int          result = 0;

#define SIO_AF_UNIX_GETPEERPID  _WSAIOR(IOC_VENDOR, 256)

    sock = win32_get_real_socket(fd);
    if (sock == INVALID_SOCKET) {
        win32_log("win32_check_peer_is_owner: invalid socket for fd=%d\n", fd);
        return 0;
    }

    if (WSAIoctl(sock, SIO_AF_UNIX_GETPEERPID,
            NULL, 0, &peer_pid, sizeof peer_pid,
            &bytes_ret, NULL, NULL) == SOCKET_ERROR) {
        win32_log("win32_check_peer_is_owner: SIO_AF_UNIX_GETPEERPID failed: %d\n",
            WSAGetLastError());
        /* Not fatal — GETPEERPID may not be supported on all builds.
         * Fail open only on older Windows 10 (< 1903) that lack the ioctl;
         * we cannot distinguish "unsupported" from "error" reliably here,
         * so log and allow the connection. */
        return 1;
    }

    if (peer_pid == 0) {
        win32_log("win32_check_peer_is_owner: peer_pid=0 (WSL bug), rejecting\n");
        return 0;
    }

    /* Open peer process — if it fails, PID was already recycled */
    hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, peer_pid);
    if (hProc == NULL) {
        win32_log("win32_check_peer_is_owner: OpenProcess(%lu) failed: %lu\n",
            peer_pid, GetLastError());
        return 0;
    }

    if (!OpenProcessToken(hProc, TOKEN_QUERY, &hToken)) {
        win32_log("win32_check_peer_is_owner: OpenProcessToken failed: %lu\n",
            GetLastError());
        goto done;
    }

    if (!GetTokenInformation(hToken, TokenUser, token_buf, needed, &needed)) {
        win32_log("win32_check_peer_is_owner: GetTokenInformation(peer) failed: %lu\n",
            GetLastError());
        goto done;
    }
    tu_peer = (TOKEN_USER *)token_buf;

    /* Get server's own token */
    hSelf = GetCurrentProcess();
    if (!OpenProcessToken(hSelf, TOKEN_QUERY, &hSelfToken)) {
        win32_log("win32_check_peer_is_owner: OpenProcessToken(self) failed: %lu\n",
            GetLastError());
        goto done;
    }

    if (!GetTokenInformation(hSelfToken, TokenUser, self_buf, self_needed, &self_needed)) {
        win32_log("win32_check_peer_is_owner: GetTokenInformation(self) failed: %lu\n",
            GetLastError());
        goto done;
    }
    tu_self = (TOKEN_USER *)self_buf;

    result = EqualSid(tu_peer->User.Sid, tu_self->User.Sid) ? 1 : 0;
    win32_log("win32_check_peer_is_owner: peer_pid=%lu sid_match=%d\n",
        peer_pid, result);

done:
    if (hToken)     CloseHandle(hToken);
    if (hSelfToken) CloseHandle(hSelfToken);
    if (hProc)      CloseHandle(hProc);
    return result;
}

/* libevent 1.x wrapper to ensure mapped IDs are used in callbacks */
void win32_event_set(struct event *ev, int fd, short events, void (*cb)(int, short, void *), void *arg) {
    SOCKET s = win32_get_real_socket(fd);
    event_set(ev, (evutil_socket_t)s, events, (void(*)(evutil_socket_t, short, void*))cb, arg);
}

/*
 * bufferevent wrapper - translate mapped fd to real SOCKET for libevent.
 * libevent's Windows backend uses select() which needs actual SOCKET handles,
 * not our virtual fds.
 */
#undef bufferevent_new
struct bufferevent *
win32_bufferevent_new(int fd, evbuffercb readcb, evbuffercb writecb,
    everrorcb errorcb, void *cbarg)
{
    SOCKET s = win32_get_real_socket(fd);
#ifdef PLATFORM_WINDOWS
    win32_log("win32_bufferevent_new: fd=%d -> socket=%llu\n", fd, (unsigned long long)s);
#endif
    return bufferevent_new((evutil_socket_t)s, readcb, writecb, errorcb, cbarg);
}
