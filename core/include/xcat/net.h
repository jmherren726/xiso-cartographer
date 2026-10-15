#ifndef XCAT_NET_H
#define XCAT_NET_H

/*
 * Platform interfaces for networking (docs/ARCHITECTURE.md §4): TCP
 * connections and a monotonic clock. The platform layer fills them in
 * (platform/host: sockets; platform/nxdk: lwIP), and tests pass fakes.
 */

#include <stddef.h>
#include <stdint.h>

#include "xcat/error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A connection; what it points to is up to the implementation. */
typedef struct xc_net_conn xc_net_conn;

typedef struct xc_net {
    /*
     * Resolves `host` (a name or an IPv4 literal) and opens a TCP connection to
     * `port`, all within `timeout_ms`. Returns XC_ERR_NET_DNS if the name doesn't
     * resolve, XC_ERR_NET_CONNECT if no address accepts, XC_ERR_NET_TIMEOUT.
     */
    xc_err (*connect)(void *ctx, const char *host, uint16_t port, uint32_t timeout_ms,
                      xc_net_conn **out);
    /* Sends all `len` bytes, or fails (XC_ERR_NET_IO, XC_ERR_NET_TIMEOUT). */
    xc_err (*send)(void *ctx, xc_net_conn *conn, const void *buf, size_t len, uint32_t timeout_ms);
    /*
     * Waits up to `timeout_ms` for data and receives 1..cap bytes. *got == 0
     * means the peer closed the connection. Returns XC_ERR_NET_TIMEOUT if nothing
     * arrived in time, XC_ERR_NET_IO if the connection failed.
     */
    xc_err (*recv)(void *ctx, xc_net_conn *conn, void *buf, size_t cap, uint32_t timeout_ms,
                   size_t *got);
    /* Closes and frees the connection. */
    void (*close)(void *ctx, xc_net_conn *conn);
    void *ctx;
} xc_net;

typedef struct xc_clock {
    uint64_t (*now_ms)(void *ctx);            /* monotonic milliseconds */
    void (*sleep_ms)(void *ctx, uint32_t ms); /* for retry backoff */
    void *ctx;
} xc_clock;

#ifdef __cplusplus
}
#endif

#endif /* XCAT_NET_H */
