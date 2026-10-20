#ifndef XCAT_HTTP_CLIENT_H
#define XCAT_HTTP_CLIENT_H

/*
 * HTTP/1.1 client for byte ranges of one remote file (docs/formats/http.md
 * §3, §6-§9): Range requests, redirects, keep-alive, retries with backoff,
 * resume after a dropped connection, and per-hop credentials (ADR 0007).
 * Everything goes through caller-provided buffers and platform interfaces
 * (xcat/net.h); nothing is allocated.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xcat/error.h"
#include "xcat/http.h"
#include "xcat/net.h"
#include "xcat/source.h"
#include "xcat/url.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XC_HTTP_MIN_BUF ((size_t)32 * 1024) /* smallest receive buffer: a full head plus room */
#define XC_HTTP_DEFAULT_TIMEOUT_MS 15000U   /* §7.1 */
#define XC_HTTP_DEFAULT_ATTEMPTS 8U         /* §7.2 */
#define XC_HTTP_BACKOFF_BASE_MS 1000U       /* §7.2 */
#define XC_HTTP_BACKOFF_MAX_MS 30000U       /* §7.2 */
#define XC_HTTP_RETRY_AFTER_MAX_MS 120000U  /* §7.2 */
#define XC_HTTP_MAX_REDIRECTS 5             /* §1 */

/*
 * Credentials hook (§8, docs/formats/credentials.md §6). Called before every
 * request hop with that hop's URL. Writes zero or more complete header lines
 * ("Name: value\r\n") into out[0, cap) and sets *len. Sets *section to the name
 * of the credentials section used (for messages), or NULL when nothing was added.
 */
typedef struct xc_http_auth {
    xc_err (*headers)(void *ctx, const xc_url *url, char *out, size_t cap, size_t *len,
                      const char **section);
    void *ctx;
} xc_http_auth;

/* Reported before each retry, so a UI can say what is happening. */
typedef struct xc_http_retry {
    xc_err err;            /* why the attempt failed */
    int status;            /* HTTP status, or 0 for a network error */
    unsigned failures;     /* consecutive failed attempts so far */
    unsigned max_attempts; /* the request gives up at this many */
    uint32_t delay_ms;     /* how long the client now waits */
} xc_http_retry;

/* Return false to give up now (the request fails with XC_ERR_CANCELLED). */
typedef bool (*xc_http_retry_fn)(void *ctx, const xc_http_retry *r);

typedef struct xc_http_config {
    const xc_net *net;        /* plain TCP; required */
    const xc_net *tls;        /* TLS connections (M5); NULL: https fails with XC_ERR_UNSUPPORTED */
    const xc_clock *clock;    /* required */
    const xc_http_auth *auth; /* NULL: no credentials */
    uint8_t *buf;             /* receive buffer, at least XC_HTTP_MIN_BUF bytes */
    size_t buf_size;
    uint32_t timeout_ms;       /* 0: XC_HTTP_DEFAULT_TIMEOUT_MS */
    unsigned max_attempts;     /* 0: XC_HTTP_DEFAULT_ATTEMPTS */
    uint32_t seed;             /* seeds the backoff jitter */
    xc_http_retry_fn on_retry; /* may be NULL */
    void *retry_ctx;
} xc_http_config;

typedef struct xc_http_client {
    xc_http_config cfg;
    uint32_t rng;
    /* The open connection, if any. */
    xc_net_conn *conn;
    const xc_net *conn_net;
    xc_url_scheme conn_scheme;
    uint16_t conn_port;
    char conn_host[XC_URL_MAX_HOST + 1];
    /* About the last failed request, for messages. */
    int last_status;          /* HTTP status of the last response, 0 if none */
    const char *last_section; /* credentials section sent with it, or NULL */
} xc_http_client;

/* Checks the config (XC_ERR_INVALID_ARG if a required part is missing). */
xc_err xc_http_client_init(xc_http_client *c, const xc_http_config *cfg);

/* Closes the open connection, if any. The client stays usable. */
void xc_http_client_close(xc_http_client *c);

/* One remote file: its URL, where redirects led, and what the server said about it. */
typedef struct xc_http_resource {
    xc_url url;       /* as given */
    xc_url effective; /* where requests go now (after redirects, §6) */
    bool size_known;
    uint64_t size;
    char etag[XC_HTTP_MAX_VALIDATOR + 1];          /* from the first response; "" if none */
    char last_modified[XC_HTTP_MAX_VALIDATOR + 1]; /* likewise */
    xc_url scratch;                                /* redirect resolution workspace */
} xc_http_resource;

/* Parses `url` (XC_ERR_URL if it isn't accepted, http.md §2). */
xc_err xc_http_resource_init(xc_http_resource *r, const char *url, size_t len);

/*
 * Learns the file's size with `Range: bytes=0-0`, following redirects. Fails
 * with XC_ERR_HTTP_NO_RANGE if the server ignores Range (ADR 0004). Does
 * nothing if the size is already known.
 */
xc_err xc_http_probe(xc_http_client *c, xc_http_resource *r);

/*
 * Receives file bytes. `offset` is the file offset of data[0]. Return XC_OK to
 * continue; any other value stops the read and is returned by xc_http_read().
 */
typedef xc_err (*xc_http_data_fn)(void *ctx, uint64_t offset, const uint8_t *data, size_t len);

/*
 * Streams [offset, offset + len) of the file to `fn`, in order, each byte once
 * (§7.3): after a dropped connection or a temporary error, it waits (§7.2) and
 * asks for the rest. Probes first if the size isn't known. XC_ERR_RANGE if the
 * range runs past the end of the file.
 */
xc_err xc_http_read(xc_http_client *c, xc_http_resource *r, uint64_t offset, uint64_t len,
                    xc_http_data_fn fn, void *ctx);

/*
 * Fetches a whole small file without Range (§3, §6): a 200 whose body is chunked or has a
 * Content-Length, after redirects. For listings and maps, which servers may generate on the
 * fly. XC_ERR_LIMIT once the body is longer than `max_len` bytes. A failure in the middle is
 * retried like a read (§7.2), and the body is then delivered again from offset 0, so `fn` must
 * accept starting over. `r`'s size and validators aren't used or set.
 */
xc_err xc_http_get(xc_http_client *c, xc_http_resource *r, uint64_t max_len, xc_http_data_fn fn,
                   void *ctx);

/* An xc_source over HTTP: every read_at, and every read_stream (a whole span), is one
 * xc_http_read. */
typedef struct xc_http_source {
    xc_http_client *client;
    xc_http_resource *res;
} xc_http_source;

/* Probes `r` and fills in `src`. `hs`, `c` and `r` must outlive `src`. */
xc_err xc_http_source_open(xc_source *src, xc_http_source *hs, xc_http_client *c,
                           xc_http_resource *r);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_HTTP_CLIENT_H */
