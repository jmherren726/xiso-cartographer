#ifndef XCAT_SNTP_H
#define XCAT_SNTP_H

/*
 * SNTP client for the time that certificate checks need (docs/formats/sntp.md, ADR 0012 §5):
 * builds the request, checks the answer, asks a server over an xc_udp, and picks the time
 * source: NTP, else the console's clock if it isn't earlier than the build time.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xcat/error.h"
#include "xcat/net.h"
#include "xcat/tls.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XC_SNTP_PACKET_BYTES 48U
#define XC_SNTP_NONCE_BYTES 8U
#define XC_SNTP_PORT 123U
#define XC_SNTP_DEFAULT_SERVER "pool.ntp.org"
#define XC_SNTP_ATTEMPTS 3U              /* sntp.md §6 */
#define XC_SNTP_DEFAULT_TIMEOUT_MS 2000U /* per attempt */

/* The request (sntp.md §2): 48 bytes, client mode, version 4, `nonce` as transmit timestamp. */
void xc_sntp_request(uint8_t out[XC_SNTP_PACKET_BYTES], const uint8_t nonce[XC_SNTP_NONCE_BYTES]);

/* An accepted answer's fields (sntp.md §3), times in Unix milliseconds. */
typedef struct xc_sntp_reply {
    int64_t transmit_ms;
    int64_t receive_ms; /* the server's Receive Timestamp, 0 if not set */
    uint8_t stratum;
    uint8_t version;
} xc_sntp_reply;

/*
 * Checks an answer against sntp.md §4.1–§4.7 and `nonce`. XC_OK and *out if accepted;
 * XC_ERR_NTP if not, with a short reason in *why (never NULL then) and, for a kiss-o'-death
 * (stratum 0), its four-letter code in kiss[0, 5) ("" otherwise). `kiss` may be NULL.
 * XC_ERR_INVALID_ARG for NULL arguments.
 */
xc_err xc_sntp_parse(const uint8_t *buf, size_t len, const uint8_t nonce[XC_SNTP_NONCE_BYTES],
                     xc_sntp_reply *out, const char **why, char kiss[5]);

/* The time at the moment an answer arrived `rtt_ms` after its request was sent (sntp.md §5). */
int64_t xc_sntp_arrival_ms(const xc_sntp_reply *reply, uint32_t rtt_ms);

/* A query's result: the time and the monotonic clock at the moment the answer arrived. */
typedef struct xc_sntp_result {
    int64_t unix_ms;
    uint64_t mono_ms;
    uint32_t rtt_ms;
    uint8_t stratum;
    unsigned attempts; /* how many requests were sent */
} xc_sntp_result;

/* Why a query failed, for the log (sntp.md §6). */
typedef struct xc_sntp_failure {
    xc_err err;        /* of the last attempt */
    const char *why;   /* for XC_ERR_NTP: which check failed; else NULL */
    char kiss[5];      /* kiss-o'-death code, or "" */
    unsigned attempts; /* requests sent */
} xc_sntp_failure;

/*
 * Asks `server`:`port` (sntp.md §6): up to XC_SNTP_ATTEMPTS requests, each with a new nonce from
 * `entropy` and `timeout_ms` to answer. The answer must not be earlier than `floor_unix_s`
 * (§4.8). XC_OK and *out, or the last error (XC_ERR_NET_DNS, XC_ERR_NET_TIMEOUT,
 * XC_ERR_NET_IO, XC_ERR_NTP, XC_ERR_TLS_NO_RANDOM) with details in *fail if not NULL.
 */
xc_err xc_sntp_query(const xc_udp *udp, const xc_clock *clock, const xc_entropy *entropy,
                     const char *server, uint16_t port, uint32_t timeout_ms, int64_t floor_unix_s,
                     xc_sntp_result *out, xc_sntp_failure *fail);

/*
 * The time source for TLS (ADR 0012 §5): NTP's answer if the query succeeded, else
 * `fallback` (the console's clock, which refuses times earlier than the build) if it gives a
 * time not earlier than `floor_unix_s`, else neither (XC_ERR_TLS_NO_TIME). After xc_time_choose,
 * xc_time_wallclock() gives an xc_wallclock reading the chosen source; `t` must outlive it.
 */
typedef enum xc_time_source_kind {
    XC_TIME_NONE,
    XC_TIME_NTP,
    XC_TIME_FALLBACK
} xc_time_source_kind;

typedef struct xc_time_source {
    xc_time_source_kind kind;
    xc_sntp_result ntp;      /* for XC_TIME_NTP */
    xc_sntp_failure failure; /* why NTP wasn't used (kind != XC_TIME_NTP and NTP was asked) */
    bool ntp_asked;          /* false when `server` was NULL (NTP turned off) */
    const xc_clock *clock;
    const xc_wallclock *fallback;
    int64_t floor_unix_s;
} xc_time_source;

/* `server` NULL skips NTP. Returns XC_OK if a source was chosen, else XC_ERR_TLS_NO_TIME. */
xc_err xc_time_choose(xc_time_source *t, const xc_udp *udp, const xc_clock *clock,
                      const xc_entropy *entropy, const char *server, uint16_t port,
                      uint32_t timeout_ms, const xc_wallclock *fallback, int64_t floor_unix_s);
void xc_time_wallclock(xc_time_source *t, xc_wallclock *out);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_SNTP_H */
