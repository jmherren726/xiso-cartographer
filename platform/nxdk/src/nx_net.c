/*
 * xc_net and xc_clock (xcat/net.h) for the Xbox: nxdk's lwIP sockets, non-blocking,
 * with lwip_select() for every timeout (nxdk builds lwIP without SO_RCVTIMEO). The
 * lwip_* names are used directly so lwIP's BSD-compatibility macros (close, send, ...)
 * never touch the xc_net fields of the same names.
 */
#include <errno.h>
#include <lwip/netdb.h>
#include <lwip/netif.h>
#include <lwip/sockets.h>
#include <nxdk/net.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include "xcat/nxdk.h"

extern struct netif *g_pnetif; /* nxdk/net.c */

struct xc_net_conn {
    int s;
};

/* --- clock ---------------------------------------------------------------------------- */

static uint64_t clock_now_ms(void *ctx)
{
    (void)ctx;
    static ULONGLONG freq;
    if (freq == 0) {
        freq = KeQueryPerformanceFrequency();
    }
    /* Split so counter * 1000 can't overflow (it would after ~7 h at 733 MHz). */
    ULONGLONG c = KeQueryPerformanceCounter();
    return (uint64_t)(c / freq * 1000U + c % freq * 1000U / freq);
}

static void clock_sleep_ms(void *ctx, uint32_t ms)
{
    (void)ctx;
    Sleep(ms);
}

void xc_nx_clock(xc_clock *clock)
{
    clock->now_ms = clock_now_ms;
    clock->sleep_ms = clock_sleep_ms;
    clock->ctx = NULL;
}

/* --- startup -------------------------------------------------------------------------- */

xc_err xc_nx_net_startup(char *ip, size_t ip_size)
{
    if (nxNetInit(NULL) != 0) {
        return XC_ERR_NET_CONNECT;
    }
    if (ip != NULL && ip_size > 0) {
        (void)snprintf(ip, ip_size, "%s", ip4addr_ntoa(netif_ip4_addr(g_pnetif)));
    }
    return XC_OK;
}

/* --- sockets -------------------------------------------------------------------------- */

typedef enum wait_for { WAIT_READ, WAIT_WRITE } wait_for;

/* Waits until `s` is readable or writable. XC_OK, XC_ERR_NET_TIMEOUT or XC_ERR_NET_IO. */
static xc_err wait_sock(int s, wait_for what, uint32_t timeout_ms)
{
    if (s < 0 || s >= FD_SETSIZE) {
        return XC_ERR_NET_IO;
    }
    uint64_t deadline = clock_now_ms(NULL) + timeout_ms;
    for (;;) {
        uint64_t now = clock_now_ms(NULL);
        uint64_t left = deadline > now ? deadline - now : 0;
        fd_set fds;
        fd_set errs;
        FD_ZERO(&fds);
        FD_ZERO(&errs);
        FD_SET(s, &fds);
        FD_SET(s, &errs); /* lwIP reports a failed connect here */
        struct timeval tv;
        tv.tv_sec = (long)(left / 1000U);
        tv.tv_usec = (long)(left % 1000U * 1000U);
        int n = lwip_select(s + 1, what == WAIT_READ ? &fds : NULL,
                            what == WAIT_WRITE ? &fds : NULL, &errs, &tv);
        if (n > 0) {
            return FD_ISSET(s, &fds) ? XC_OK : XC_ERR_NET_IO;
        }
        if (n == 0) {
            return XC_ERR_NET_TIMEOUT;
        }
        if (errno != EINTR) {
            return XC_ERR_NET_IO;
        }
    }
}

static bool would_block(void)
{
    return errno == EINPROGRESS || errno == EWOULDBLOCK || errno == EAGAIN;
}

static xc_err net_connect(void *ctx, const char *host, uint16_t port, uint32_t timeout_ms,
                          xc_net_conn **out)
{
    (void)ctx;
    char port_str[8];
    (void)snprintf(port_str, sizeof port_str, "%u", (unsigned)port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET; /* URLs are IPv4 or names (http.md §2.1) */
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    struct addrinfo *ai = NULL;
    if (lwip_getaddrinfo(host, port_str, &hints, &ai) != 0 || ai == NULL) {
        return XC_ERR_NET_DNS;
    }
    xc_net_conn *conn = malloc(sizeof *conn);
    int s = lwip_socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    xc_err err = conn == NULL ? XC_ERR_NOMEM : XC_OK;
    if (err == XC_OK && (s < 0 || lwip_fcntl(s, F_SETFL, O_NONBLOCK) != 0)) {
        err = XC_ERR_NET_CONNECT;
    }
    if (err == XC_OK && lwip_connect(s, ai->ai_addr, ai->ai_addrlen) != 0) {
        if (!would_block()) {
            err = XC_ERR_NET_CONNECT;
        } else {
            err = wait_sock(s, WAIT_WRITE, timeout_ms);
            if (err == XC_ERR_NET_IO) {
                err = XC_ERR_NET_CONNECT;
            }
            int so_err = 0;
            socklen_t len = (socklen_t)sizeof so_err;
            if (err == XC_OK &&
                (lwip_getsockopt(s, SOL_SOCKET, SO_ERROR, &so_err, &len) != 0 || so_err != 0)) {
                err = XC_ERR_NET_CONNECT;
            }
        }
    }
    lwip_freeaddrinfo(ai);
    if (err != XC_OK) {
        if (s >= 0) {
            (void)lwip_close(s);
        }
        free(conn);
        return err;
    }
    conn->s = s;
    *out = conn;
    return XC_OK;
}

static xc_err net_send(void *ctx, xc_net_conn *conn, const void *buf, size_t len,
                       uint32_t timeout_ms)
{
    (void)ctx;
    const char *p = buf;
    while (len > 0) {
        ssize_t n = lwip_send(conn->s, p, len, 0);
        if (n > 0) {
            p += n;
            len -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
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
    for (;;) {
        xc_err err = wait_sock(conn->s, WAIT_READ, timeout_ms);
        if (err != XC_OK) {
            return err;
        }
        ssize_t n = lwip_recv(conn->s, buf, cap, 0);
        if (n >= 0) {
            *got = (size_t)n; /* 0: the peer closed the connection */
            return XC_OK;
        }
        if (errno != EINTR && !would_block()) {
            return XC_ERR_NET_IO;
        }
    }
}

static void net_close(void *ctx, xc_net_conn *conn)
{
    (void)ctx;
    if (conn != NULL) {
        (void)lwip_close(conn->s);
        free(conn);
    }
}

void xc_nx_net(xc_net *net)
{
    net->connect = net_connect;
    net->send = net_send;
    net->recv = net_recv;
    net->close = net_close;
    net->ctx = NULL;
}
