/*
 * xc_net and xc_clock (xcat/net.h) for PCs: non-blocking sockets with select()
 * for every timeout, so one code path serves Winsock and BSD sockets.
 */
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>
typedef SOCKET sock_t;
#define BAD_SOCK INVALID_SOCKET
#define CLOSE_SOCK closesocket
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
typedef int sock_t;
#define BAD_SOCK (-1)
#define CLOSE_SOCK close
#endif

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xcat/host.h"

struct xc_net_conn {
    sock_t s;
};

/* --- clock ---------------------------------------------------------------------------- */

static uint64_t clock_now_ms(void *ctx)
{
    (void)ctx;
#if defined(_WIN32)
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
#endif
}

static void clock_sleep_ms(void *ctx, uint32_t ms)
{
    (void)ctx;
#if defined(_WIN32)
    Sleep(ms);
#else
    struct timespec ts = {(time_t)(ms / 1000U), (long)(ms % 1000U) * 1000000L};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
#endif
}

void xc_host_clock(xc_clock *clock)
{
    clock->now_ms = clock_now_ms;
    clock->sleep_ms = clock_sleep_ms;
    clock->ctx = NULL;
}

/* --- sockets -------------------------------------------------------------------------- */

xc_err xc_host_net_startup(void)
{
#if defined(_WIN32)
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0 ? XC_OK : XC_ERR_NET_CONNECT;
#else
    return XC_OK;
#endif
}

void xc_host_net_cleanup(void)
{
#if defined(_WIN32)
    (void)WSACleanup();
#endif
}

static bool set_nonblocking(sock_t s)
{
#if defined(_WIN32)
    u_long on = 1;
    return ioctlsocket(s, (long)FIONBIO, &on) == 0;
#else
    int flags = fcntl(s, F_GETFL, 0);
    return flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

static bool would_block(void)
{
#if defined(_WIN32)
    int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
#else
    return errno == EINPROGRESS || errno == EWOULDBLOCK || errno == EAGAIN;
#endif
}

static bool interrupted(void)
{
#if defined(_WIN32)
    return WSAGetLastError() == WSAEINTR;
#else
    return errno == EINTR;
#endif
}

typedef enum wait_for { WAIT_READ, WAIT_WRITE } wait_for;

/* Waits until `s` is readable or writable. XC_OK, XC_ERR_NET_TIMEOUT or XC_ERR_NET_IO. */
static xc_err wait_sock(sock_t s, wait_for what, uint32_t timeout_ms)
{
#if !defined(_WIN32)
    if (s >= FD_SETSIZE) {
        return XC_ERR_NET_IO;
    }
#endif
    uint64_t deadline = clock_now_ms(NULL) + timeout_ms;
    for (;;) {
        uint64_t now = clock_now_ms(NULL);
        uint64_t left = deadline > now ? deadline - now : 0;
        fd_set fds;
        fd_set errs;
        FD_ZERO(&fds);
        FD_ZERO(&errs);
        FD_SET(s, &fds);
        FD_SET(s, &errs); /* Winsock reports a failed connect here */
        struct timeval tv;
#if defined(_WIN32)
        tv.tv_sec = (long)(left / 1000U);
#else
        tv.tv_sec = (time_t)(left / 1000U);
#endif
        tv.tv_usec = (int)(left % 1000U * 1000U); /* < 1,000,000: fits every tv_usec type */
#if defined(_WIN32)
        int n = select(0, what == WAIT_READ ? &fds : NULL, what == WAIT_WRITE ? &fds : NULL, &errs,
                       &tv);
#else
        int n = select(s + 1, what == WAIT_READ ? &fds : NULL, what == WAIT_WRITE ? &fds : NULL,
                       &errs, &tv);
#endif
        if (n > 0) {
            return FD_ISSET(s, &fds) ? XC_OK : XC_ERR_NET_IO;
        }
        if (n == 0) {
            return XC_ERR_NET_TIMEOUT;
        }
        if (!interrupted()) {
            return XC_ERR_NET_IO;
        }
    }
}

/* Connects one address within `timeout_ms`. */
static xc_err connect_one(const struct addrinfo *ai, uint32_t timeout_ms, sock_t *out)
{
    sock_t s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (s == BAD_SOCK) {
        return XC_ERR_NET_CONNECT;
    }
    if (!set_nonblocking(s)) {
        (void)CLOSE_SOCK(s);
        return XC_ERR_NET_CONNECT;
    }
#if defined(_WIN32)
    int rc = connect(s, ai->ai_addr, (int)ai->ai_addrlen);
#else
    int rc = connect(s, ai->ai_addr, ai->ai_addrlen);
#endif
    xc_err err = XC_OK;
    if (rc != 0) {
        if (!would_block()) {
            err = XC_ERR_NET_CONNECT;
        } else {
            err = wait_sock(s, WAIT_WRITE, timeout_ms);
            if (err == XC_ERR_NET_IO) {
                err = XC_ERR_NET_CONNECT;
            }
            if (err == XC_OK) {
                int so_err = 0;
                socklen_t len = (socklen_t)sizeof so_err;
                if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&so_err, &len) != 0 ||
                    so_err != 0) {
                    err = XC_ERR_NET_CONNECT;
                }
            }
        }
    }
    if (err != XC_OK) {
        (void)CLOSE_SOCK(s);
        return err;
    }
    *out = s;
    return XC_OK;
}

static xc_err net_connect(void *ctx, const char *host, uint16_t port, uint32_t timeout_ms,
                          xc_net_conn **out)
{
    (void)ctx;
    char port_str[8];
    (void)snprintf(port_str, sizeof port_str, "%u", (unsigned)port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    struct addrinfo *list = NULL;
    if (getaddrinfo(host, port_str, &hints, &list) != 0 || list == NULL) {
        return XC_ERR_NET_DNS;
    }
    xc_net_conn *conn = malloc(sizeof *conn);
    if (conn == NULL) {
        freeaddrinfo(list);
        return XC_ERR_NOMEM;
    }
    /* Try each address in turn within one budget, IPv4 first like the console (IPv4 only):
     * "localhost" may list ::1 first, and Windows takes ~2 s to report a refused connect. */
    uint64_t deadline = clock_now_ms(NULL) + timeout_ms;
    xc_err err = XC_ERR_NET_CONNECT;
    for (int pass = 0; pass < 2 && err != XC_OK; pass++) {
        for (const struct addrinfo *ai = list; ai != NULL; ai = ai->ai_next) {
            if ((ai->ai_family == AF_INET) != (pass == 0)) {
                continue;
            }
            uint64_t now = clock_now_ms(NULL);
            if (now >= deadline) {
                err = XC_ERR_NET_TIMEOUT;
                break;
            }
            err = connect_one(ai, (uint32_t)(deadline - now), &conn->s);
            if (err == XC_OK) {
                break;
            }
        }
    }
    freeaddrinfo(list);
    if (err != XC_OK) {
        free(conn);
        return err;
    }
    *out = conn;
    return XC_OK;
}

static xc_err net_send(void *ctx, xc_net_conn *conn, const void *buf, size_t len,
                       uint32_t timeout_ms)
{
    (void)ctx;
    const char *p = buf;
    while (len > 0) {
        int chunk = len > (size_t)INT_MAX ? INT_MAX : (int)len;
#if defined(_WIN32)
        int n = send(conn->s, p, chunk, 0);
#elif defined(MSG_NOSIGNAL)
        /* A peer that has gone must not raise SIGPIPE and kill the process. */
        ssize_t n = send(conn->s, p, (size_t)chunk, MSG_NOSIGNAL);
#else
        ssize_t n = send(conn->s, p, (size_t)chunk, 0);
#endif
        if (n > 0) {
            p += n;
            len -= (size_t)n;
            continue;
        }
        if (n < 0 && interrupted()) {
            continue;
        }
        if (n == 0 || !would_block()) {
            return XC_ERR_NET_IO;
        }
        xc_err err = wait_sock(conn->s, WAIT_WRITE, timeout_ms);
        if (err != XC_OK) {
            return err;
        }
    }
    return XC_OK;
}

static xc_err net_recv(void *ctx, xc_net_conn *conn, void *buf, size_t cap, uint32_t timeout_ms,
                       size_t *got)
{
    (void)ctx;
    *got = 0;
    int chunk = cap > (size_t)INT_MAX ? INT_MAX : (int)cap;
    for (;;) {
        xc_err err = wait_sock(conn->s, WAIT_READ, timeout_ms);
        if (err != XC_OK) {
            return err;
        }
#if defined(_WIN32)
        int n = recv(conn->s, buf, chunk, 0);
#else
        ssize_t n = recv(conn->s, buf, (size_t)chunk, 0);
#endif
        if (n >= 0) {
            *got = (size_t)n; /* 0: the peer closed the connection */
            return XC_OK;
        }
        if (!interrupted() && !would_block()) {
            return XC_ERR_NET_IO;
        }
    }
}

static void net_close(void *ctx, xc_net_conn *conn)
{
    (void)ctx;
    if (conn != NULL) {
        (void)CLOSE_SOCK(conn->s);
        free(conn);
    }
}

void xc_host_net(xc_net *net)
{
    net->connect = net_connect;
    net->send = net_send;
    net->recv = net_recv;
    net->close = net_close;
    net->ctx = NULL;
}

/* --- UDP ------------------------------------------------------------------------------ */

/* Sends `req` on a UDP socket connected to `ai` (so only that address's datagrams arrive) and
 * waits for one answer. */
static xc_err udp_one(const struct addrinfo *ai, const void *req, size_t req_len, void *resp,
                      size_t cap, uint32_t timeout_ms, size_t *got, uint32_t *rtt_ms)
{
    sock_t s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (s == BAD_SOCK) {
        return XC_ERR_NET_IO;
    }
#if defined(_WIN32)
    bool ok = set_nonblocking(s) && connect(s, ai->ai_addr, (int)ai->ai_addrlen) == 0 &&
              send(s, req, (int)req_len, 0) == (int)req_len;
#else
    bool ok = set_nonblocking(s) && connect(s, ai->ai_addr, ai->ai_addrlen) == 0 &&
              send(s, req, req_len, 0) == (ssize_t)req_len;
#endif
    if (!ok) {
        (void)CLOSE_SOCK(s);
        return XC_ERR_NET_IO;
    }
    uint64_t sent = clock_now_ms(NULL);
    uint64_t deadline = sent + timeout_ms;
    int chunk = cap > (size_t)INT_MAX ? INT_MAX : (int)cap;
    xc_err err = XC_ERR_NET_TIMEOUT;
    for (;;) {
        uint64_t now = clock_now_ms(NULL);
        if (now >= deadline) {
            break;
        }
        err = wait_sock(s, WAIT_READ, (uint32_t)(deadline - now));
        if (err != XC_OK) {
            break;
        }
#if defined(_WIN32)
        int n = recv(s, resp, chunk, 0);
#else
        ssize_t n = recv(s, resp, (size_t)chunk, 0);
#endif
        if (n >= 0) {
            *got = (size_t)n;
            *rtt_ms = (uint32_t)(clock_now_ms(NULL) - sent);
            break;
        }
#if defined(_WIN32)
        if (WSAGetLastError() == WSAEMSGSIZE) { /* longer than cap: the start was copied */
            *got = cap;
            *rtt_ms = (uint32_t)(clock_now_ms(NULL) - sent);
            break;
        }
#endif
        if (!interrupted() && !would_block()) {
            err = XC_ERR_NET_IO; /* e.g. the host answered "port unreachable" */
            break;
        }
        err = XC_ERR_NET_TIMEOUT;
    }
    (void)CLOSE_SOCK(s);
    return err;
}

static xc_err udp_exchange(void *ctx, const char *host, uint16_t port, const void *req,
                           size_t req_len, void *resp, size_t cap, uint32_t timeout_ms, size_t *got,
                           uint32_t *rtt_ms)
{
    (void)ctx;
    *got = 0;
    *rtt_ms = 0;
    char port_str[8];
    (void)snprintf(port_str, sizeof port_str, "%u", (unsigned)port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    struct addrinfo *list = NULL;
    if (getaddrinfo(host, port_str, &hints, &list) != 0 || list == NULL) {
        return XC_ERR_NET_DNS;
    }
    /* IPv4 first, like the console (IPv4 only) and like net_connect. */
    const struct addrinfo *pick = list;
    for (const struct addrinfo *ai = list; ai != NULL; ai = ai->ai_next) {
        if (ai->ai_family == AF_INET) {
            pick = ai;
            break;
        }
    }
    xc_err err = udp_one(pick, req, req_len, resp, cap, timeout_ms, got, rtt_ms);
    freeaddrinfo(list);
    return err;
}

void xc_host_udp(xc_udp *udp)
{
    udp->exchange = udp_exchange;
    udp->ctx = NULL;
}
