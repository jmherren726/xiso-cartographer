/*
 * HTTP/1.1 Range client: docs/formats/http.md §3 (requests), §6 (status codes),
 * §7 (retries and resume), §8 (credentials) and §9 (connections).
 */
#include "xcat/http_client.h"

#include <string.h>

#include "xcat/version.h"

#define USER_AGENT_SUFFIX " (+https://github.com/jmherren726/xiso-cartographer)"

/* --- small helpers ------------------------------------------------------------------ */

/* Appends to a fixed buffer; remembers overflow instead of failing each call. */
typedef struct wbuf {
    char *p;
    size_t cap;
    size_t n;
    bool overflow;
} wbuf;

static void w_mem(wbuf *w, const char *s, size_t len)
{
    if (w->overflow || len > w->cap - w->n) {
        w->overflow = true;
        return;
    }
    memcpy(w->p + w->n, s, len);
    w->n += len;
}

static void w_str(wbuf *w, const char *s)
{
    w_mem(w, s, strlen(s));
}

static void w_u64(wbuf *w, uint64_t v)
{
    char tmp[20];
    size_t i = sizeof tmp;
    do {
        tmp[--i] = (char)('0' + (int)(v % 10U));
        v /= 10U;
    } while (v > 0);
    w_mem(w, tmp + i, sizeof tmp - i);
}

static uint32_t next_rand(xc_http_client *c)
{
    /* xorshift32: only for spreading retries out, never for anything secret. */
    uint32_t x = c->rng;
    x ^= x << 13U;
    x ^= x >> 17U;
    x ^= x << 5U;
    c->rng = x;
    return x;
}

static bool same_url(const xc_url *a, const xc_url *b)
{
    return a->scheme == b->scheme && a->port == b->port && strcmp(a->host, b->host) == 0 &&
           strcmp(a->target, b->target) == 0;
}

/* --- client ----------------------------------------------------------------------------- */

xc_err xc_http_client_init(xc_http_client *c, const xc_http_config *cfg)
{
    if (c == NULL || cfg == NULL || cfg->net == NULL || cfg->clock == NULL || cfg->buf == NULL ||
        cfg->buf_size < XC_HTTP_MIN_BUF) {
        return XC_ERR_INVALID_ARG;
    }
    memset(c, 0, sizeof *c);
    c->cfg = *cfg;
    if (c->cfg.timeout_ms == 0) {
        c->cfg.timeout_ms = XC_HTTP_DEFAULT_TIMEOUT_MS;
    }
    if (c->cfg.max_attempts == 0) {
        c->cfg.max_attempts = XC_HTTP_DEFAULT_ATTEMPTS;
    }
    c->rng = cfg->seed != 0 ? cfg->seed : 0x9E3779B9U;
    return XC_OK;
}

void xc_http_client_close(xc_http_client *c)
{
    if (c != NULL && c->conn != NULL) {
        c->conn_net->close(c->conn_net->ctx, c->conn);
        c->conn = NULL;
        c->conn_net = NULL;
    }
}

/* §9: reuse the open connection for the same scheme, host and port. */
static xc_err ensure_conn(xc_http_client *c, const xc_url *u, bool *reused)
{
    const xc_net *net = u->scheme == XC_URL_HTTPS ? c->cfg.tls : c->cfg.net;
    *reused = false;
    if (net == NULL) {
        return XC_ERR_UNSUPPORTED;
    }
    if (c->conn != NULL && c->conn_net == net && c->conn_scheme == u->scheme &&
        c->conn_port == u->port && strcmp(c->conn_host, u->host) == 0) {
        *reused = true;
        return XC_OK;
    }
    xc_http_client_close(c);
    xc_net_conn *conn = NULL;
    xc_err err = net->connect(net->ctx, u->host, u->port, c->cfg.timeout_ms, &conn);
    if (err != XC_OK) {
        return err;
    }
    c->conn = conn;
    c->conn_net = net;
    c->conn_scheme = u->scheme;
    c->conn_port = u->port;
    memcpy(c->conn_host, u->host, sizeof c->conn_host);
    return XC_OK;
}

xc_err xc_http_resource_init(xc_http_resource *r, const char *url, size_t len)
{
    if (r == NULL || url == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    memset(r, 0, sizeof *r);
    xc_err err = xc_url_parse(url, len, &r->url);
    if (err == XC_OK) {
        r->effective = r->url;
    }
    return err;
}

/* One request/response exchange (plus a silent reconnect if a kept-alive connection had died). */
typedef struct attempt {
    uint64_t first; /* requested range */
    uint64_t last;
    xc_http_data_fn fn;
    void *ctx;

    uint64_t delivered;   /* bytes passed to fn */
    bool retryable;       /* the failure is temporary (§7.2) */
    bool redirect;        /* r->scratch holds the next URL */
    bool empty;           /* the file is empty (416 with total 0, probe only) */
    bool callback_failed; /* fn returned an error */
    bool has_retry_after;
    uint32_t retry_after_ms;
} attempt;

/* §3: the request, into the start of the receive buffer. */
static xc_err build_request(xc_http_client *c, const xc_url *u, const attempt *a, size_t *len)
{
    wbuf w = {(char *)c->cfg.buf, c->cfg.buf_size, 0, false};
    w_str(&w, "GET ");
    w_str(&w, u->target);
    w_str(&w, " HTTP/1.1\r\nHost: ");
    w_str(&w, u->host);
    if (!xc_url_default_port(u)) {
        w_str(&w, ":");
        w_u64(&w, u->port);
    }
    w_str(&w, "\r\nUser-Agent: xiso-cartographer/");
    w_str(&w, xc_version_string());
    w_str(&w, USER_AGENT_SUFFIX "\r\nAccept-Encoding: identity\r\nRange: bytes=");
    w_u64(&w, a->first);
    w_str(&w, "-");
    w_u64(&w, a->last);
    w_str(&w, "\r\n");
    c->last_section = NULL;
    if (c->cfg.auth != NULL && !w.overflow && w.cap - w.n > 2) {
        size_t added = 0;
        const char *section = NULL;
        xc_err err =
            c->cfg.auth->headers(c->cfg.auth->ctx, u, w.p + w.n, w.cap - w.n - 2, &added, &section);
        if (err != XC_OK) {
            return err;
        }
        if (added > w.cap - w.n - 2) {
            return XC_ERR_INVALID_ARG;
        }
        w.n += added;
        c->last_section = added > 0 ? section : NULL;
    }
    w_str(&w, "\r\n");
    if (w.overflow) {
        return XC_ERR_LIMIT;
    }
    *len = w.n;
    return XC_OK;
}

/* Sends the request and reads until the head is complete. *n = bytes in the buffer. */
static xc_err exchange(xc_http_client *c, const xc_url *u, attempt *a, size_t *n, size_t *head_len)
{
    for (int tries = 0;; tries++) {
        bool reused = false;
        xc_err err = ensure_conn(c, u, &reused);
        if (err != XC_OK) {
            a->retryable = err != XC_ERR_UNSUPPORTED;
            return err;
        }
        size_t req_len = 0;
        err = build_request(c, u, a, &req_len);
        if (err != XC_OK) {
            return err;
        }
        err = c->conn_net->send(c->conn_net->ctx, c->conn, c->cfg.buf, req_len, c->cfg.timeout_ms);
        size_t have = 0;
        while (err == XC_OK) {
            size_t got = 0;
            err = c->conn_net->recv(c->conn_net->ctx, c->conn, c->cfg.buf + have,
                                    c->cfg.buf_size - have, c->cfg.timeout_ms, &got);
            if (err == XC_OK && got == 0) {
                err = XC_ERR_NET_IO; /* closed before the head was complete */
            }
            if (err != XC_OK) {
                break;
            }
            have += got;
            size_t end = xc_http_head_end(c->cfg.buf, have);
            if (end > 0) {
                *n = have;
                *head_len = end;
                return XC_OK;
            }
            if (have >= XC_HTTP_MAX_HEAD) {
                xc_http_client_close(c);
                return XC_ERR_HTTP_PROTOCOL;
            }
        }
        xc_http_client_close(c);
        if (!(reused && tries == 0 && have == 0)) {
            a->retryable = true;
            return err;
        }
        /* A kept-alive connection the server had already closed: reconnect once, quietly. */
    }
}

static xc_err deliver(xc_http_client *c, attempt *a, const uint8_t *data, size_t len)
{
    xc_err err = a->fn(a->ctx, a->first + a->delivered, data, len);
    a->delivered += len;
    if (err != XC_OK) {
        a->callback_failed = true;
        xc_http_client_close(c);
    }
    return err;
}

/* §5: streams the body of a 206 to the callback. The first bytes are in buf[pos, have). */
static xc_err read_body(xc_http_client *c, attempt *a, const xc_http_response *resp, size_t pos,
                        size_t have)
{
    uint64_t remaining = resp->range_last - resp->range_first + 1U;
    xc_http_chunked ch;
    xc_http_chunked_init(&ch);
    uint8_t *buf = c->cfg.buf;
    for (;;) {
        if (resp->chunked) {
            while (pos < have && !xc_http_chunked_done(&ch)) {
                size_t used = 0;
                const uint8_t *data = NULL;
                size_t data_len = 0;
                xc_err err =
                    xc_http_chunked_feed(&ch, buf + pos, have - pos, &used, &data, &data_len);
                if (err != XC_OK || (uint64_t)data_len > remaining) {
                    xc_http_client_close(c);
                    return XC_ERR_HTTP_PROTOCOL;
                }
                pos += used;
                if (data_len > 0) {
                    remaining -= data_len;
                    err = deliver(c, a, data, data_len);
                    if (err != XC_OK) {
                        return err;
                    }
                }
            }
            if (xc_http_chunked_done(&ch)) {
                if (remaining != 0) {
                    xc_http_client_close(c);
                    return XC_ERR_HTTP_PROTOCOL;
                }
                break;
            }
        } else {
            size_t take = have - pos;
            if ((uint64_t)take > remaining) {
                take = (size_t)remaining;
            }
            if (take > 0) {
                remaining -= take;
                xc_err err = deliver(c, a, buf + pos, take);
                if (err != XC_OK) {
                    return err;
                }
                pos += take;
            }
            if (remaining == 0) {
                break;
            }
        }
        size_t got = 0;
        xc_err err = c->conn_net->recv(c->conn_net->ctx, c->conn, buf, c->cfg.buf_size,
                                       c->cfg.timeout_ms, &got);
        if (err == XC_OK && got == 0) {
            err = XC_ERR_NET_IO; /* closed before the body was complete */
        }
        if (err != XC_OK) {
            xc_http_client_close(c);
            a->retryable = true;
            return err;
        }
        pos = 0;
        have = got;
    }
    /* §9: bytes after the body, or Connection: close, end the connection. */
    if (pos < have || resp->close) {
        xc_http_client_close(c);
    }
    return XC_OK;
}

static bool is_redirect(int s)
{
    return s == 301 || s == 302 || s == 303 || s == 307 || s == 308;
}

static bool is_temporary(int s)
{
    return s == 408 || s == 429 || s == 500 || s == 502 || s == 503 || s == 504;
}

/* §6: a 206 must answer exactly what was asked, about the same file. */
static xc_err check_partial(xc_http_resource *r, const attempt *a, const xc_http_response *resp)
{
    if (!resp->has_range || !resp->has_total || resp->range_first != a->first ||
        resp->range_last > a->last) {
        return XC_ERR_HTTP_PROTOCOL;
    }
    if (resp->has_length && resp->length != resp->range_last - resp->range_first + 1U) {
        return XC_ERR_HTTP_PROTOCOL;
    }
    if (!r->size_known) {
        r->size_known = true;
        r->size = resp->range_total;
        memcpy(r->etag, resp->etag, sizeof r->etag);
        memcpy(r->last_modified, resp->last_modified, sizeof r->last_modified);
        return XC_OK;
    }
    /* §7.3: a different size, or a different validator on both sides, means a new file. */
    if (resp->range_total != r->size ||
        (r->etag[0] != '\0' && resp->etag[0] != '\0' && strcmp(r->etag, resp->etag) != 0) ||
        (r->last_modified[0] != '\0' && resp->last_modified[0] != '\0' &&
         strcmp(r->last_modified, resp->last_modified) != 0)) {
        return XC_ERR_HTTP_CHANGED;
    }
    return XC_OK;
}

static xc_err run_attempt(xc_http_client *c, xc_http_resource *r, attempt *a)
{
    size_t have = 0;
    size_t head_len = 0;
    c->last_status = 0;
    xc_err err = exchange(c, &r->effective, a, &have, &head_len);
    if (err != XC_OK) {
        return err;
    }
    xc_http_response resp;
    err = xc_http_parse_head(c->cfg.buf, head_len, &resp);
    if (err != XC_OK) {
        xc_http_client_close(c);
        return err;
    }
    c->last_status = resp.status;
    if (resp.status == 206) {
        err = check_partial(r, a, &resp);
        if (err != XC_OK) {
            xc_http_client_close(c);
            return err;
        }
        return read_body(c, a, &resp, head_len, have);
    }

    /* Anything but a 206: its body isn't wanted, so the connection can't be reused. */
    xc_http_client_close(c);
    if (is_redirect(resp.status)) {
        if (resp.location == NULL ||
            xc_url_resolve(&r->effective, resp.location, resp.location_len, &r->scratch) != XC_OK) {
            return XC_ERR_HTTP_PROTOCOL;
        }
        a->redirect = true;
        return XC_OK;
    }
    switch (resp.status) {
    case 200:
        return XC_ERR_HTTP_NO_RANGE;
    case 401:
    case 403:
        return XC_ERR_HTTP_DENIED;
    case 404:
    case 410:
        return XC_ERR_HTTP_NOT_FOUND;
    case 416:
        if (resp.range_unsatisfied && r->size_known && resp.range_total != r->size) {
            return XC_ERR_HTTP_CHANGED;
        }
        if (resp.range_unsatisfied && !r->size_known && resp.range_total == 0 && a->first == 0) {
            r->size_known = true; /* an empty file: bytes=0-0 can't be satisfied */
            r->size = 0;
            a->empty = true;
            return XC_OK;
        }
        return XC_ERR_RANGE;
    default:
        break;
    }
    if (is_temporary(resp.status)) {
        a->retryable = true;
        if (resp.has_retry_after) {
            uint64_t ms = (uint64_t)resp.retry_after_s * 1000U;
            a->has_retry_after = true;
            a->retry_after_ms =
                ms > XC_HTTP_RETRY_AFTER_MAX_MS ? XC_HTTP_RETRY_AFTER_MAX_MS : (uint32_t)ms;
        }
    }
    return XC_ERR_HTTP_STATUS;
}

/* §7.2: min(base * 2^(n-1), max), then a random value in [half, all] of it. */
static uint32_t backoff_ms(xc_http_client *c, unsigned failures)
{
    uint32_t d = XC_HTTP_BACKOFF_BASE_MS;
    for (unsigned i = 1; i < failures && d < XC_HTTP_BACKOFF_MAX_MS; i++) {
        d *= 2U;
    }
    if (d > XC_HTTP_BACKOFF_MAX_MS) {
        d = XC_HTTP_BACKOFF_MAX_MS;
    }
    uint32_t lo = d / 2U;
    return lo + next_rand(c) % (d - lo + 1U);
}

/* Streams [first, last] with redirects, retries and resume. */
static xc_err fetch(xc_http_client *c, xc_http_resource *r, uint64_t first, uint64_t last,
                    xc_http_data_fn fn, void *ctx)
{
    uint64_t pos = first;
    unsigned failures = 0;
    unsigned redirects = 0;
    bool restarted = false;
    for (;;) {
        attempt a;
        memset(&a, 0, sizeof a);
        a.first = pos;
        a.last = last;
        a.fn = fn;
        a.ctx = ctx;
        xc_err err = run_attempt(c, r, &a);
        pos += a.delivered;
        if (a.delivered > 0) {
            failures = 0;
            redirects = 0;
        }
        if (a.callback_failed) {
            return err;
        }
        if (err == XC_OK && a.redirect) {
            if (++redirects > XC_HTTP_MAX_REDIRECTS) {
                return XC_ERR_HTTP_REDIRECTS;
            }
            memcpy(&r->effective, &r->scratch, sizeof r->effective);
            continue;
        }
        if (err == XC_OK && (a.empty || pos > last)) {
            return XC_OK;
        }
        if (err == XC_OK) {
            continue; /* the server sent less than asked for (§6): ask for the rest */
        }
        if (pos > last && a.retryable) {
            return XC_OK; /* every byte arrived; only the end of a chunked body was lost */
        }
        if (!a.retryable) {
            /* A redirect target that went away (e.g. an expired storage node): start over once. */
            if (err == XC_ERR_HTTP_NOT_FOUND && !restarted && !same_url(&r->url, &r->effective)) {
                restarted = true;
                redirects = 0;
                memcpy(&r->effective, &r->url, sizeof r->effective);
                continue;
            }
            return err;
        }
        if (++failures >= c->cfg.max_attempts) {
            return err;
        }
        xc_http_retry info;
        info.err = err;
        info.status = c->last_status;
        info.failures = failures;
        info.max_attempts = c->cfg.max_attempts;
        info.delay_ms = a.has_retry_after ? a.retry_after_ms : backoff_ms(c, failures);
        if (c->cfg.on_retry != NULL && !c->cfg.on_retry(c->cfg.retry_ctx, &info)) {
            return XC_ERR_CANCELLED;
        }
        c->cfg.clock->sleep_ms(c->cfg.clock->ctx, info.delay_ms);
    }
}

static xc_err discard(void *ctx, uint64_t offset, const uint8_t *data, size_t len)
{
    (void)ctx;
    (void)offset;
    (void)data;
    (void)len;
    return XC_OK;
}

xc_err xc_http_probe(xc_http_client *c, xc_http_resource *r)
{
    if (c == NULL || r == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    if (r->size_known) {
        return XC_OK;
    }
    return fetch(c, r, 0, 0, discard, NULL);
}

xc_err xc_http_read(xc_http_client *c, xc_http_resource *r, uint64_t offset, uint64_t len,
                    xc_http_data_fn fn, void *ctx)
{
    if (c == NULL || r == NULL || fn == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    xc_err err = xc_http_probe(c, r);
    if (err != XC_OK) {
        return err;
    }
    if (offset > r->size || len > r->size - offset) {
        return XC_ERR_RANGE;
    }
    if (len == 0) {
        return XC_OK;
    }
    return fetch(c, r, offset, offset + len - 1U, fn, ctx);
}

/* --- xc_source adapter ------------------------------------------------------------------- */

typedef struct copy_ctx {
    uint8_t *out;
    uint64_t base;
} copy_ctx;

static xc_err copy_out(void *ctx, uint64_t offset, const uint8_t *data, size_t len)
{
    copy_ctx *cc = ctx;
    memcpy(cc->out + (size_t)(offset - cc->base), data, len);
    return XC_OK;
}

static xc_err source_read_at(void *ctx, uint64_t offset, void *buf, size_t len)
{
    xc_http_source *hs = ctx;
    copy_ctx cc = {buf, offset};
    return xc_http_read(hs->client, hs->res, offset, len, copy_out, &cc);
}

xc_err xc_http_source_open(xc_source *src, xc_http_source *hs, xc_http_client *c,
                           xc_http_resource *r)
{
    if (src == NULL || hs == NULL || c == NULL || r == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    xc_err err = xc_http_probe(c, r);
    if (err != XC_OK) {
        return err;
    }
    hs->client = c;
    hs->res = r;
    src->read_at = source_read_at;
    src->ctx = hs;
    src->size = r->size;
    return XC_OK;
}
