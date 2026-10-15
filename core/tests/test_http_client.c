/*
 * Tests for the HTTP Range client (xcat/http_client.h, docs/formats/http.md
 * §3, §6-§9) against a scripted in-memory network and clock: each test installs
 * a handler that answers requests and can inject faults (drops, stalls, wrong
 * ranges, redirects, temporary errors).
 */
#include "unity.h"
#include "xcat/http_client.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FILE_SIZE 100000U
#define MAX_CONNS 4
#define OUT_CAP (FILE_SIZE + 64U * 1024U)
#define MAX_LOG 64

static uint8_t file_byte(uint64_t i)
{
    return (uint8_t)((i * 31U + (i >> 9U)) & 0xFFU);
}

/* One parsed request, as the fake server saw it. */
typedef struct req {
    char host[64];
    unsigned port;
    char target[512];
    uint64_t first;
    uint64_t last;
    bool has_range;
    char auth[128]; /* the Authorization header, if any */
    bool has_ua;
    bool identity;
} req;

typedef struct fconn {
    bool used;
    char host[64];
    unsigned port;
    char rq[8192];
    size_t rq_len;
    uint8_t out[OUT_CAP];
    size_t out_len;
    size_t out_pos;
    size_t fail_at;  /* when out_pos reaches this, fail with fail_err */
    xc_err fail_err; /* XC_OK here means the server closes (EOF) */
    bool has_fail;
    bool close_after; /* EOF once the queued response is sent */
    bool dead;        /* a kept-alive connection the server already closed */
} fconn;

struct fake;
typedef void (*handler_fn)(struct fake *f, fconn *c, const req *r);

typedef struct fake {
    fconn conns[MAX_CONNS];
    handler_fn handler;
    int connects;
    int connect_failures; /* fail this many connects first */
    xc_err connect_err;
    req log[MAX_LOG];
    int nlog;
    uint64_t now;
    uint32_t sleeps[MAX_LOG];
    int nsleeps;
    int counter; /* free for handlers */
    uint64_t size;
} fake;

static fake F;

/* --- fake server helpers ---------------------------------------------------------------- */

static void out_mem(fconn *c, const void *p, size_t n)
{
    TEST_ASSERT_TRUE(c->out_len + n <= OUT_CAP);
    memcpy(c->out + c->out_len, p, n);
    c->out_len += n;
}

static void out_fmt(fconn *c, const char *fmt, ...)
{
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    TEST_ASSERT_TRUE(n >= 0 && (size_t)n < sizeof tmp);
    out_mem(c, tmp, (size_t)n);
}

static void out_body(fconn *c, uint64_t first, uint64_t last)
{
    for (uint64_t i = first; i <= last; i++) {
        uint8_t b = file_byte(i);
        out_mem(c, &b, 1);
    }
}

typedef enum framing { CL, CHUNKED, UNTIL_CLOSE } framing;

static void serve(fconn *c, uint64_t first, uint64_t last, uint64_t total, framing fr,
                  const char *extra)
{
    out_fmt(c,
            "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes %" PRIu64 "-%" PRIu64 "/%" PRIu64
            "\r\n%s",
            first, last, total, extra);
    if (fr == CL) {
        out_fmt(c, "Content-Length: %" PRIu64 "\r\n\r\n", last - first + 1U);
        out_body(c, first, last);
    } else if (fr == CHUNKED) {
        out_fmt(c, "Transfer-Encoding: chunked\r\n\r\n");
        for (uint64_t p = first; p <= last;) {
            uint64_t n = last - p + 1U < 3001U ? last - p + 1U : 3001U;
            out_fmt(c, "%" PRIx64 ";x=y\r\n", n);
            out_body(c, p, p + n - 1U);
            out_fmt(c, "\r\n");
            p += n;
        }
        out_fmt(c, "0\r\nX-Trailer: 1\r\n\r\n");
    } else {
        out_fmt(c, "Connection: close\r\n\r\n");
        out_body(c, first, last);
        c->close_after = true;
    }
}

/* The usual answer: exactly the range asked for, with Content-Length. */
static void serve_req(fake *f, fconn *c, const req *r)
{
    TEST_ASSERT_TRUE(r->has_range);
    serve(c, r->first, r->last < f->size ? r->last : f->size - 1U, f->size, CL, "ETag: \"v1\"\r\n");
}

static void parse_req(const char *text, req *r)
{
    memset(r, 0, sizeof *r);
    TEST_ASSERT_EQUAL_INT(0, strncmp(text, "GET ", 4));
    const char *sp = strchr(text + 4, ' ');
    TEST_ASSERT_NOT_NULL(sp);
    TEST_ASSERT_TRUE((size_t)(sp - text - 4) < sizeof r->target);
    memcpy(r->target, text + 4, (size_t)(sp - text - 4));
    TEST_ASSERT_EQUAL_INT(0, strncmp(sp, " HTTP/1.1\r\n", 11));
    for (const char *line = strstr(text, "\r\n") + 2; strncmp(line, "\r\n", 2) != 0;
         line = strstr(line, "\r\n") + 2) {
        if (strncmp(line, "Range: bytes=", 13) == 0) {
            char *dash = NULL;
            char *end = NULL;
            r->first = strtoull(line + 13, &dash, 10);
            TEST_ASSERT_EQUAL_CHAR('-', *dash);
            r->last = strtoull(dash + 1, &end, 10);
            TEST_ASSERT_EQUAL_CHAR('\r', *end);
            r->has_range = true;
        } else if (strncmp(line, "Host: ", 6) == 0) {
            const char *e = strstr(line, "\r\n");
            size_t n = (size_t)(e - line - 6);
            TEST_ASSERT_TRUE(n < sizeof r->host);
            memcpy(r->host, line + 6, n);
        } else if (strncmp(line, "Authorization: ", 15) == 0) {
            const char *e = strstr(line, "\r\n");
            memcpy(r->auth, line + 15, (size_t)(e - line - 15));
        } else if (strncmp(line, "User-Agent: xiso-cartographer/", 30) == 0) {
            r->has_ua = true;
        } else if (strncmp(line, "Accept-Encoding: identity\r\n", 27) == 0) {
            r->identity = true;
        }
    }
}

/* --- xc_net / xc_clock over the fake -------------------------------------------------- */

static xc_err f_connect(void *ctx, const char *host, uint16_t port, uint32_t timeout_ms,
                        xc_net_conn **out)
{
    fake *f = ctx;
    TEST_ASSERT_TRUE(timeout_ms > 0);
    f->connects++;
    if (f->connect_failures > 0) {
        f->connect_failures--;
        return f->connect_err;
    }
    fconn *c = NULL;
    for (int i = 0; i < MAX_CONNS && c == NULL; i++) {
        if (!f->conns[i].used) {
            c = &f->conns[i];
        }
    }
    TEST_ASSERT_NOT_NULL_MESSAGE(c, "too many open connections (leak?)");
    memset(c, 0, sizeof *c);
    c->used = true;
    (void)snprintf(c->host, sizeof c->host, "%s", host);
    c->port = port;
    *out = (xc_net_conn *)c;
    return XC_OK;
}

static xc_err f_send(void *ctx, xc_net_conn *conn, const void *buf, size_t len, uint32_t t)
{
    fake *f = ctx;
    fconn *c = (fconn *)conn;
    (void)t;
    TEST_ASSERT_TRUE(c->rq_len + len < sizeof c->rq);
    memcpy(c->rq + c->rq_len, buf, len);
    c->rq_len += len;
    c->rq[c->rq_len] = '\0';
    char *end = strstr(c->rq, "\r\n\r\n");
    if (end != NULL && !c->dead) {
        TEST_ASSERT_EQUAL_PTR(c->rq + c->rq_len - 4, end); /* one request at a time */
        TEST_ASSERT_TRUE(f->nlog < MAX_LOG);
        req *r = &f->log[f->nlog++];
        parse_req(c->rq, r);
        r->port = c->port;
        c->rq_len = 0;
        c->out_len = 0;
        c->out_pos = 0;
        c->has_fail = false;
        f->handler(f, c, r);
    }
    return XC_OK;
}

static xc_err f_recv(void *ctx, xc_net_conn *conn, void *buf, size_t cap, uint32_t t, size_t *got)
{
    (void)ctx;
    (void)t;
    fconn *c = (fconn *)conn;
    *got = 0;
    if (c->dead) {
        return XC_OK; /* EOF */
    }
    if (c->has_fail && c->out_pos >= c->fail_at) {
        return c->fail_err;
    }
    if (c->out_pos == c->out_len) {
        return c->close_after ? XC_OK : XC_ERR_NET_TIMEOUT;
    }
    size_t n = c->out_len - c->out_pos;
    if (n > cap) {
        n = cap;
    }
    if (n > 1500) {
        n = 1500; /* packet-sized pieces exercise the incremental parsing */
    }
    if (c->has_fail && c->out_pos + n > c->fail_at) {
        n = c->fail_at - c->out_pos;
    }
    memcpy(buf, c->out + c->out_pos, n);
    c->out_pos += n;
    *got = n;
    return XC_OK;
}

static void f_close(void *ctx, xc_net_conn *conn)
{
    (void)ctx;
    fconn *c = (fconn *)conn;
    TEST_ASSERT_TRUE(c->used);
    c->used = false;
}

static uint64_t f_now(void *ctx)
{
    return ((fake *)ctx)->now;
}

static void f_sleep(void *ctx, uint32_t ms)
{
    fake *f = ctx;
    TEST_ASSERT_TRUE(f->nsleeps < MAX_LOG);
    f->sleeps[f->nsleeps++] = ms;
    f->now += ms;
}

static const xc_net fnet = {f_connect, f_send, f_recv, f_close, &F};
static const xc_clock fclock = {f_now, f_sleep, &F};

/* --- per-test setup ----------------------------------------------------------------------- */

static uint8_t rbuf[64 * 1024];
static xc_http_client client;
static xc_http_resource res;
static uint8_t got[FILE_SIZE];
static uint64_t got_len;

static xc_err collect(void *ctx, uint64_t offset, const uint8_t *data, size_t len)
{
    (void)ctx;
    TEST_ASSERT_EQUAL_UINT64(got_len, offset); /* in order, each byte once */
    TEST_ASSERT_TRUE(offset + len <= FILE_SIZE);
    memcpy(got + offset, data, len);
    got_len += len;
    return XC_OK;
}

static void check_got(uint64_t first, uint64_t len)
{
    TEST_ASSERT_EQUAL_UINT64(first + len, got_len);
    for (uint64_t i = first; i < first + len; i++) {
        if (got[i] != file_byte(i)) {
            char msg[64];
            (void)snprintf(msg, sizeof msg, "byte %" PRIu64, i);
            TEST_FAIL_MESSAGE(msg);
        }
    }
}

static xc_http_config base_cfg(void)
{
    xc_http_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.net = &fnet;
    cfg.clock = &fclock;
    cfg.buf = rbuf;
    cfg.buf_size = sizeof rbuf;
    cfg.seed = 1234;
    return cfg;
}

static void start(handler_fn h, const char *url)
{
    xc_http_client_close(&client);
    memset(&F, 0, sizeof F);
    F.handler = h;
    F.size = FILE_SIZE;
    F.connect_err = XC_ERR_NET_CONNECT;
    xc_http_config cfg = base_cfg();
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_http_client_init(&client, &cfg));
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_http_resource_init(&res, url, strlen(url)));
    got_len = 0;
}

/* got[] is filled from offset 0; reads starting later pre-set got_len. */
static xc_err read_all(uint64_t offset, uint64_t len)
{
    got_len = offset;
    return xc_http_read(&client, &res, offset, len, collect, NULL);
}

void setUp(void) {}

void tearDown(void)
{
    xc_http_client_close(&client);
    for (int i = 0; i < MAX_CONNS; i++) {
        TEST_ASSERT_FALSE_MESSAGE(F.conns[i].used, "connection left open");
    }
}

/* --- tests ------------------------------------------------------------------------------- */

static void test_probe_and_read(void)
{
    start(serve_req, "http://192.168.1.20:8080/xbox/Halo (Europe).iso");
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_http_probe(&client, &res));
    TEST_ASSERT_TRUE(res.size_known);
    TEST_ASSERT_EQUAL_UINT64(FILE_SIZE, res.size);
    TEST_ASSERT_EQUAL_STRING("\"v1\"", res.etag);
    TEST_ASSERT_EQUAL_INT(1, F.nlog);
    TEST_ASSERT_EQUAL_STRING("/xbox/Halo%20(Europe).iso", F.log[0].target);
    TEST_ASSERT_EQUAL_STRING("192.168.1.20:8080", F.log[0].host);
    TEST_ASSERT_TRUE(F.log[0].has_ua && F.log[0].identity);
    TEST_ASSERT_EQUAL_UINT64(0, F.log[0].first);
    TEST_ASSERT_EQUAL_UINT64(0, F.log[0].last);

    TEST_ASSERT_EQUAL_INT(XC_OK, read_all(0, FILE_SIZE));
    check_got(0, FILE_SIZE);
    TEST_ASSERT_EQUAL_INT(XC_OK, read_all(5000, 123));
    check_got(5000, 123);
    TEST_ASSERT_EQUAL_INT(1, F.connects); /* kept alive throughout (§9) */
    TEST_ASSERT_EQUAL_INT(3, F.nlog);
    TEST_ASSERT_EQUAL_UINT64(5122, F.log[2].last);

    TEST_ASSERT_EQUAL_INT(XC_ERR_RANGE, read_all(FILE_SIZE - 1U, 2));
    TEST_ASSERT_EQUAL_INT(XC_OK, read_all(FILE_SIZE, 0));
    TEST_ASSERT_EQUAL_INT(0, F.nsleeps);
}

static void serve_chunked(fake *f, fconn *c, const req *r)
{
    serve(c, r->first, r->last, f->size, CHUNKED, "");
}

static void serve_until_close(fake *f, fconn *c, const req *r)
{
    serve(c, r->first, r->last, f->size, UNTIL_CLOSE, "");
}

static void test_body_framings(void)
{
    start(serve_chunked, "http://h/f");
    TEST_ASSERT_EQUAL_INT(XC_OK, read_all(0, FILE_SIZE));
    check_got(0, FILE_SIZE);
    TEST_ASSERT_EQUAL_INT(XC_OK, read_all(10, 20));
    check_got(10, 20);
    TEST_ASSERT_EQUAL_INT(1, F.connects);
    xc_http_client_close(&client);

    start(serve_until_close, "http://h/f");
    TEST_ASSERT_EQUAL_INT(XC_OK, read_all(0, FILE_SIZE));
    check_got(0, FILE_SIZE);
    TEST_ASSERT_EQUAL_INT(2, F.connects); /* probe, then read: Connection: close each time */
    TEST_ASSERT_EQUAL_INT(0, F.nsleeps);
}

/* archive.org-style: a cross-host redirect, then relative and absolute-path ones. */
static void serve_redirects(fake *f, fconn *c, const req *r)
{
    if (strcmp(c->host, "archive.example") == 0) {
        out_fmt(c, "HTTP/1.1 302 Found\r\nLocation: http://dn1.storage.example:81/a/b/x.iso"
                   "\r\nContent-Length: 5\r\n\r\nhello");
    } else if (strcmp(r->target, "/a/b/x.iso") == 0) {
        out_fmt(c, "HTTP/1.1 307 Temporary Redirect\r\nLocation: ../c/y.iso\r\n\r\n");
    } else if (strcmp(r->target, "/a/c/y.iso") == 0) {
        out_fmt(c, "HTTP/1.1 301 Moved\r\nLocation: /final.iso#frag\r\n\r\n");
    } else {
        TEST_ASSERT_EQUAL_STRING("/final.iso", r->target);
        serve_req(f, c, r);
    }
}

static void test_redirects(void)
{
    start(serve_redirects, "http://archive.example/download/x.iso");
    TEST_ASSERT_EQUAL_INT(XC_OK, read_all(100, 1000));
    check_got(100, 1000);
    TEST_ASSERT_EQUAL_INT(5, F.nlog); /* 3 redirects + the probe, then the read */
    TEST_ASSERT_EQUAL_STRING("dn1.storage.example:81", F.log[1].host);
    TEST_ASSERT_EQUAL_STRING("dn1.storage.example", res.effective.host);
    TEST_ASSERT_EQUAL_STRING("/final.iso", res.effective.target);
    /* Later reads go straight to the final URL. */
    TEST_ASSERT_EQUAL_INT(XC_OK, read_all(0, 10));
    TEST_ASSERT_EQUAL_INT(6, F.nlog);
    TEST_ASSERT_EQUAL_STRING("/final.iso", F.log[5].target);
}

static void serve_loop(fake *f, fconn *c, const req *r)
{
    (void)f;
    (void)r;
    out_fmt(c, "HTTP/1.1 302 Found\r\nLocation: /again\r\n\r\n");
}

static void serve_bad_location(fake *f, fconn *c, const req *r)
{
    (void)f;
    (void)r;
    out_fmt(c, "HTTP/1.1 302 Found\r\nLocation: ftp://elsewhere/x\r\n\r\n");
}

static void test_redirect_limits(void)
{
    start(serve_loop, "http://h/f");
    TEST_ASSERT_EQUAL_INT(XC_ERR_HTTP_REDIRECTS, xc_http_probe(&client, &res));
    TEST_ASSERT_EQUAL_INT(XC_HTTP_MAX_REDIRECTS + 1, F.nlog);
    xc_http_client_close(&client);
    start(serve_bad_location, "http://h/f");
    TEST_ASSERT_EQUAL_INT(XC_ERR_HTTP_PROTOCOL, xc_http_probe(&client, &res));
    TEST_ASSERT_EQUAL_INT(0, F.nsleeps);
}

static int g_status;
static const char *g_extra;

static void serve_status(fake *f, fconn *c, const req *r)
{
    (void)f;
    (void)r;
    out_fmt(c, "HTTP/1.1 %d Whatever\r\nContent-Length: 0\r\n%s\r\n", g_status,
            g_extra != NULL ? g_extra : "");
}

static void test_status_codes(void)
{
    static const struct {
        int status;
        const char *extra;
        xc_err want;
        int requests;
    } cases[] = {
        {200, NULL, XC_ERR_HTTP_NO_RANGE, 1},
        {401, NULL, XC_ERR_HTTP_DENIED, 1},
        {403, NULL, XC_ERR_HTTP_DENIED, 1},
        {404, NULL, XC_ERR_HTTP_NOT_FOUND, 1},
        {410, NULL, XC_ERR_HTTP_NOT_FOUND, 1},
        {416, "Content-Range: bytes */5\r\n", XC_ERR_RANGE, 1},
        {418, NULL, XC_ERR_HTTP_STATUS, 1},
        {503, NULL, XC_ERR_HTTP_STATUS, (int)XC_HTTP_DEFAULT_ATTEMPTS},
        {206, NULL, XC_ERR_HTTP_PROTOCOL, 1}, /* no Content-Range */
        {206, "Content-Range: bytes 0-0/*\r\n", XC_ERR_HTTP_PROTOCOL, 1},
        {206, "Content-Range: bytes 1-1/9\r\n", XC_ERR_HTTP_PROTOCOL, 1},
        {206, "Content-Range: bytes 0-1/9\r\n", XC_ERR_HTTP_PROTOCOL, 1},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        start(serve_status, "http://h/f");
        g_status = cases[i].status;
        g_extra = cases[i].extra;
        char msg[32];
        (void)snprintf(msg, sizeof msg, "case %zu (%d)", i, g_status);
        TEST_ASSERT_EQUAL_INT_MESSAGE(cases[i].want, xc_http_probe(&client, &res), msg);
        TEST_ASSERT_EQUAL_INT_MESSAGE(cases[i].requests, F.nlog, msg);
        if (g_status != 206) {
            TEST_ASSERT_EQUAL_INT_MESSAGE(g_status, client.last_status, msg);
        }
        xc_http_client_close(&client);
    }
    /* An empty file answers bytes=0-0 with 416 and a total of 0. */
    start(serve_status, "http://h/empty");
    g_status = 416;
    g_extra = "Content-Range: bytes */0\r\n";
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_http_probe(&client, &res));
    TEST_ASSERT_TRUE(res.size_known);
    TEST_ASSERT_EQUAL_UINT64(0, res.size);
    TEST_ASSERT_EQUAL_INT(XC_OK, read_all(0, 0));
}

/* §7.2: backoff doubles from 1 s to 30 s, each value jittered into [d/2, d]. */
static void test_backoff_and_give_up(void)
{
    start(serve_req, "http://h/f");
    F.connect_failures = 100;
    TEST_ASSERT_EQUAL_INT(XC_ERR_NET_CONNECT, xc_http_probe(&client, &res));
    TEST_ASSERT_EQUAL_INT((int)XC_HTTP_DEFAULT_ATTEMPTS, F.connects);
    TEST_ASSERT_EQUAL_INT((int)XC_HTTP_DEFAULT_ATTEMPTS - 1, F.nsleeps);
    static const uint32_t caps[] = {1000, 2000, 4000, 8000, 16000, 30000, 30000};
    for (int i = 0; i < F.nsleeps; i++) {
        TEST_ASSERT_TRUE(F.sleeps[i] >= caps[i] / 2U && F.sleeps[i] <= caps[i]);
    }

    /* A few failures, then the server is back: the probe succeeds. */
    start(serve_req, "http://h/f");
    F.connect_failures = 3;
    F.connect_err = XC_ERR_NET_DNS;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_http_probe(&client, &res));
    TEST_ASSERT_EQUAL_INT(3, F.nsleeps);
}

static void serve_busy_then_ok(fake *f, fconn *c, const req *r)
{
    if (f->counter++ < 2) {
        out_fmt(c, "HTTP/1.1 503 Busy\r\nRetry-After: 7\r\nContent-Length: 0\r\n\r\n");
    } else {
        serve_req(f, c, r);
    }
}

static void test_retry_after(void)
{
    start(serve_busy_then_ok, "http://h/f");
    TEST_ASSERT_EQUAL_INT(XC_OK, read_all(0, 50));
    check_got(0, 50);
    TEST_ASSERT_EQUAL_INT(2, F.nsleeps);
    TEST_ASSERT_EQUAL_UINT32(7000, F.sleeps[0]);
    TEST_ASSERT_EQUAL_UINT32(7000, F.sleeps[1]);
}

/* Drops the connection partway through every response that would carry the byte at `cut`. */
static uint64_t g_cut;
static xc_err g_cut_err;

static void serve_and_drop(fake *f, fconn *c, const req *r)
{
    serve_req(f, c, r);
    if (r->first <= g_cut && g_cut <= r->last && f->counter++ < 3) {
        c->has_fail = true;
        c->fail_at = c->out_len - (size_t)(r->last - g_cut) - 1U; /* everything before `cut` */
        c->fail_err = g_cut_err;
    }
}

static int g_drop_retries;

/* A cut 206 body is reported as a network error, without the 206's status. */
static bool expect_net_retry(void *ctx, const xc_http_retry *r)
{
    (void)ctx;
    g_drop_retries++;
    TEST_ASSERT_EQUAL_INT(0, r->status);
    TEST_ASSERT_TRUE(r->err == XC_ERR_NET_IO || r->err == XC_ERR_NET_TIMEOUT);
    return true;
}

static void test_resume_after_drop(void)
{
    static const xc_err errs[] = {XC_OK /* EOF */, XC_ERR_NET_IO, XC_ERR_NET_TIMEOUT};
    for (size_t i = 0; i < sizeof errs / sizeof errs[0]; i++) {
        start(serve_and_drop, "http://h/f");
        xc_http_config cfg = base_cfg();
        cfg.on_retry = expect_net_retry;
        TEST_ASSERT_EQUAL_INT(XC_OK, xc_http_client_init(&client, &cfg));
        g_drop_retries = 0;
        g_cut = 54321;
        g_cut_err = errs[i];
        TEST_ASSERT_EQUAL_INT(XC_OK, read_all(0, FILE_SIZE));
        TEST_ASSERT_EQUAL_INT(3, g_drop_retries);
        check_got(0, FILE_SIZE);
        /* probe, then three cut attempts each resuming exactly at the cut, then success. */
        TEST_ASSERT_EQUAL_INT(5, F.nlog);
        for (int k = 2; k < 5; k++) {
            TEST_ASSERT_EQUAL_UINT64(g_cut, F.log[k].first);
            TEST_ASSERT_EQUAL_UINT64(FILE_SIZE - 1U, F.log[k].last);
        }
        TEST_ASSERT_EQUAL_INT(3, F.nsleeps);
        xc_http_client_close(&client);
    }
}

/* Progress resets the failure count (§7.2): many outages, each after some bytes. */
static void serve_drop_often(fake *f, fconn *c, const req *r)
{
    serve_req(f, c, r);
    if (r->last - r->first > 2000U) {
        c->has_fail = true;
        c->fail_at = c->out_len - (size_t)(r->last - r->first) + 1000U;
        c->fail_err = XC_ERR_NET_IO;
    }
    (void)f;
}

static void test_failures_reset_on_progress(void)
{
    start(serve_drop_often, "http://h/f");
    TEST_ASSERT_EQUAL_INT(XC_OK, read_all(0, 20000));
    check_got(0, 20000);
    TEST_ASSERT_TRUE(F.nsleeps > (int)XC_HTTP_DEFAULT_ATTEMPTS); /* survived more than 8 drops */
    for (int i = 0; i < F.nsleeps; i++) {
        TEST_ASSERT_TRUE(F.sleeps[i] <= 1000U); /* always the first backoff step */
    }
}

/* The server caps every response at 4096 bytes: the client asks for the rest (§6). */
static void serve_short(fake *f, fconn *c, const req *r)
{
    uint64_t last = r->last - r->first >= 4096U ? r->first + 4095U : r->last;
    serve(c, r->first, last, f->size, CL, "");
}

static void test_short_ranges(void)
{
    start(serve_short, "http://h/f");
    TEST_ASSERT_EQUAL_INT(XC_OK, read_all(1, 10000));
    check_got(1, 10000);
    TEST_ASSERT_EQUAL_INT(1 + 3, F.nlog);
    TEST_ASSERT_EQUAL_UINT64(1 + 4096, F.log[2].first);
    TEST_ASSERT_EQUAL_INT(0, F.nsleeps);
}

static const char *g_etag;
static uint64_t g_total;

static void serve_changing(fake *f, fconn *c, const req *r)
{
    char extra[64];
    (void)snprintf(extra, sizeof extra, "ETag: %s\r\n", f->counter++ == 0 ? "\"v1\"" : g_etag);
    serve(c, r->first, r->last, f->counter == 1 ? f->size : g_total, CL, extra);
}

static void test_changed_file(void)
{
    start(serve_changing, "http://h/f");
    g_etag = "\"v2\"";
    g_total = FILE_SIZE;
    TEST_ASSERT_EQUAL_INT(XC_ERR_HTTP_CHANGED, read_all(0, 10));
    start(serve_changing, "http://h/f");
    g_etag = "\"v1\"";
    g_total = FILE_SIZE + 1U;
    TEST_ASSERT_EQUAL_INT(XC_ERR_HTTP_CHANGED, read_all(0, 10));
    start(serve_changing, "http://h/f");
    g_etag = "\"v1\"";
    g_total = FILE_SIZE;
    TEST_ASSERT_EQUAL_INT(XC_OK, read_all(0, 10));
}

static void serve_close_idle(fake *f, fconn *c, const req *r)
{
    serve_req(f, c, r);
    /* After this response the server silently drops the idle connection. */
    f->counter++;
}

static void test_stale_keepalive(void)
{
    start(serve_close_idle, "http://h/f");
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_http_probe(&client, &res));
    for (int i = 0; i < MAX_CONNS; i++) {
        if (F.conns[i].used) {
            F.conns[i].dead = true;
        }
    }
    TEST_ASSERT_EQUAL_INT(XC_OK, read_all(0, 100));
    check_got(0, 100);
    TEST_ASSERT_EQUAL_INT(2, F.connects);
    TEST_ASSERT_EQUAL_INT(0, F.nsleeps); /* not counted as a failure */
}

static void serve_trailing_garbage(fake *f, fconn *c, const req *r)
{
    serve_req(f, c, r);
    out_fmt(c, "EXTRA");
}

static void test_bytes_after_body_close(void)
{
    start(serve_trailing_garbage, "http://h/f");
    TEST_ASSERT_EQUAL_INT(XC_OK, read_all(0, 100));
    TEST_ASSERT_EQUAL_INT(XC_OK, read_all(0, 100));
    TEST_ASSERT_EQUAL_INT(3, F.connects); /* never reused */
}

static int g_auth_calls;
static char g_auth_hosts[8][XC_URL_MAX_HOST + 1];

static xc_err auth_headers(void *ctx, const xc_url *url, char *out, size_t cap, size_t *len,
                           const char **section)
{
    (void)ctx;
    TEST_ASSERT_TRUE(g_auth_calls < 8);
    (void)snprintf(g_auth_hosts[g_auth_calls++], sizeof g_auth_hosts[0], "%s", url->host);
    *len = 0;
    *section = NULL;
    if (strcmp(url->host, "dn1.storage.example") == 0) {
        int n = snprintf(out, cap, "Authorization: Bearer secret\r\n");
        TEST_ASSERT_TRUE(n > 0 && (size_t)n < cap);
        *len = (size_t)n;
        *section = "storage";
    }
    return XC_OK;
}

static void serve_auth(fake *f, fconn *c, const req *r)
{
    if (strcmp(c->host, "archive.example") == 0) {
        TEST_ASSERT_EQUAL_STRING("", r->auth);
        out_fmt(c, "HTTP/1.1 302 Found\r\nLocation: http://dn1.storage.example/x\r\n\r\n");
    } else if (f->counter++ == 0) {
        TEST_ASSERT_EQUAL_STRING("Bearer secret", r->auth);
        out_fmt(c, "HTTP/1.1 403 Forbidden\r\n\r\n");
    }
}

/* §8: credentials are decided per hop, and a denial names the section used. */
static void test_credentials_per_hop(void)
{
    start(serve_auth, "http://archive.example/x");
    static const xc_http_auth auth = {auth_headers, NULL};
    xc_http_config cfg = base_cfg();
    cfg.auth = &auth;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_http_client_init(&client, &cfg));
    g_auth_calls = 0;
    TEST_ASSERT_EQUAL_INT(XC_ERR_HTTP_DENIED, xc_http_probe(&client, &res));
    TEST_ASSERT_EQUAL_INT(2, g_auth_calls);
    TEST_ASSERT_EQUAL_STRING("archive.example", g_auth_hosts[0]);
    TEST_ASSERT_EQUAL_STRING("dn1.storage.example", g_auth_hosts[1]);
    TEST_ASSERT_EQUAL_INT(403, client.last_status);
    TEST_ASSERT_EQUAL_STRING("storage", client.last_section);
    TEST_ASSERT_EQUAL_INT(0, F.nsleeps); /* ADR 0006: never retried */
}

static void serve_gone_storage(fake *f, fconn *c, const req *r)
{
    if (strcmp(c->host, "archive.example") == 0) {
        char loc[64];
        (void)snprintf(loc, sizeof loc, "http://dn%d.example/x", f->counter);
        out_fmt(c, "HTTP/1.1 302 Found\r\nLocation: %s\r\n\r\n", loc);
    } else if (strcmp(c->host, "dn0.example") == 0 && f->counter > 0) {
        out_fmt(c, "HTTP/1.1 404 Not Found\r\n\r\n");
    } else {
        f->counter++;
        serve_req(f, c, r);
    }
}

/* A redirect target that disappears sends the client back to the original URL, once. */
static void test_expired_redirect(void)
{
    start(serve_gone_storage, "http://archive.example/x");
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_http_probe(&client, &res));
    TEST_ASSERT_EQUAL_STRING("dn0.example", res.effective.host);
    TEST_ASSERT_EQUAL_INT(XC_OK, read_all(0, 10));
    check_got(0, 10);
    TEST_ASSERT_EQUAL_STRING("dn1.example", res.effective.host);
}

static int g_retry_calls;

static bool cancel_retry(void *ctx, const xc_http_retry *r)
{
    (void)ctx;
    g_retry_calls++;
    TEST_ASSERT_EQUAL_INT(XC_ERR_NET_CONNECT, r->err);
    TEST_ASSERT_EQUAL_UINT(1, r->failures);
    TEST_ASSERT_EQUAL_UINT(XC_HTTP_DEFAULT_ATTEMPTS, r->max_attempts);
    return false;
}

static xc_err stop_at_100(void *ctx, uint64_t offset, const uint8_t *data, size_t len)
{
    (void)ctx;
    (void)data;
    return offset + len >= 100U ? XC_ERR_CANCELLED : XC_OK;
}

static void test_cancel_and_unsupported(void)
{
    start(serve_req, "http://h/f");
    xc_http_config cfg = base_cfg();
    cfg.on_retry = cancel_retry;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_http_client_init(&client, &cfg));
    F.connect_failures = 1;
    g_retry_calls = 0;
    TEST_ASSERT_EQUAL_INT(XC_ERR_CANCELLED, xc_http_probe(&client, &res));
    TEST_ASSERT_EQUAL_INT(1, g_retry_calls);
    TEST_ASSERT_EQUAL_INT(0, F.nsleeps);

    TEST_ASSERT_EQUAL_INT(XC_ERR_CANCELLED,
                          xc_http_read(&client, &res, 0, FILE_SIZE, stop_at_100, NULL));
    TEST_ASSERT_EQUAL_INT(0, F.nsleeps);

    start(serve_req, "https://h/f");
    TEST_ASSERT_EQUAL_INT(XC_ERR_UNSUPPORTED, xc_http_probe(&client, &res));
    TEST_ASSERT_EQUAL_INT(0, F.connects);

    xc_http_config bad = base_cfg();
    bad.buf_size = XC_HTTP_MIN_BUF - 1U;
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_http_client_init(&client, &bad));
    TEST_ASSERT_EQUAL_INT(XC_ERR_URL, xc_http_resource_init(&res, "http://u@h/", 11));
}

static void serve_huge_head(fake *f, fconn *c, const req *r)
{
    (void)f;
    (void)r;
    out_fmt(c, "HTTP/1.1 206 x\r\n");
    while (c->out_len < XC_HTTP_MAX_HEAD + 100U) {
        out_fmt(c, "X-Padding: 0123456789012345678901234567890123456789\r\n");
    }
    out_fmt(c, "\r\n");
}

static void test_head_too_large(void)
{
    start(serve_huge_head, "http://h/f");
    TEST_ASSERT_EQUAL_INT(XC_ERR_HTTP_PROTOCOL, xc_http_probe(&client, &res));
    TEST_ASSERT_EQUAL_INT(0, F.nsleeps);
}

static void test_source_adapter(void)
{
    start(serve_req, "http://h/f");
    xc_source src;
    xc_http_source hs;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_http_source_open(&src, &hs, &client, &res));
    TEST_ASSERT_EQUAL_UINT64(FILE_SIZE, src.size);
    uint8_t buf[300];
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_source_read(&src, 70000, buf, sizeof buf));
    for (size_t i = 0; i < sizeof buf; i++) {
        TEST_ASSERT_EQUAL_UINT8(file_byte(70000 + i), buf[i]);
    }
    TEST_ASSERT_EQUAL_INT(XC_ERR_RANGE, xc_source_read(&src, FILE_SIZE - 10U, buf, 11));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_probe_and_read);
    RUN_TEST(test_body_framings);
    RUN_TEST(test_redirects);
    RUN_TEST(test_redirect_limits);
    RUN_TEST(test_status_codes);
    RUN_TEST(test_backoff_and_give_up);
    RUN_TEST(test_retry_after);
    RUN_TEST(test_resume_after_drop);
    RUN_TEST(test_failures_reset_on_progress);
    RUN_TEST(test_short_ranges);
    RUN_TEST(test_changed_file);
    RUN_TEST(test_stale_keepalive);
    RUN_TEST(test_bytes_after_body_close);
    RUN_TEST(test_credentials_per_hop);
    RUN_TEST(test_expired_redirect);
    RUN_TEST(test_cancel_and_unsupported);
    RUN_TEST(test_head_too_large);
    RUN_TEST(test_source_adapter);
    return UNITY_END();
}
