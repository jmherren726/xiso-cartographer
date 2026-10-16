/*
 * Tests for xcat/tls.h (ADR 0005, ADR 0012): trust anchors from PEM, and the TLS net against
 * a BearSSL server running in this process behind a fake TCP xc_net. argv[1] is the folder of
 * test certificates (tools/fixtures/tls/, made by make_certs.sh: valid 2026-01-01..2036-01-01
 * for test.example and localhost).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bearssl.h"
#include "unity.h"
#include "xcat/http_client.h"
#include "xcat/tls.h"

static const char *certs_dir = "tools/fixtures/tls";

#define T_VALID 1767225600LL      /* 2026-01-01 00:00:00 UTC: the leaf's notBefore */
#define T_NOW 1790000000LL        /* 2026-09-21 */
#define T_EXPIRED 2082758401LL    /* one second past the leaf's notAfter (2036-01-01) */
#define T_RESET_CLOCK 978307200LL /* 2001-01-01, a console whose clock battery ran out */

/* ---- Files --------------------------------------------------------------------------- */

static char *read_file(const char *name, size_t *len)
{
    char path[1024];
    (void)snprintf(path, sizeof path, "%s/%s", certs_dir, name);
    FILE *f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    char *data = malloc((size_t)64 * 1024);
    TEST_ASSERT_NOT_NULL(data);
    *len = fread(data, 1, (size_t)64 * 1024, f);
    (void)fclose(f);
    return data;
}

/* Decodes every PEM object in `pem` into DER (for the test server's chain). */
typedef struct der_list {
    unsigned char data[4][4096];
    size_t len[4];
    size_t count;
} der_list;

static void der_out(void *ctx, const void *buf, size_t len)
{
    der_list *d = ctx;
    size_t i = d->count - 1;
    TEST_ASSERT_TRUE(d->len[i] + len <= sizeof d->data[i]);
    memcpy(d->data[i] + d->len[i], buf, len);
    d->len[i] += len;
}

static void pem_to_der(const char *pem, size_t len, der_list *d)
{
    br_pem_decoder_context pc;
    br_pem_decoder_init(&pc);
    memset(d, 0, sizeof *d);
    while (len > 0) {
        size_t n = br_pem_decoder_push(&pc, pem, len);
        pem += n;
        len -= n;
        int ev = br_pem_decoder_event(&pc);
        if (ev == BR_PEM_BEGIN_OBJ) {
            TEST_ASSERT_TRUE(d->count < 4);
            d->count++;
            br_pem_decoder_setdest(&pc, der_out, d);
        }
        TEST_ASSERT_NOT_EQUAL(BR_PEM_ERROR, ev);
    }
}

/* ---- An in-process TLS server behind a fake TCP net ----------------------------------- */

struct xc_net_conn {
    int unused;
};

typedef struct server {
    br_ssl_server_context sc;
    unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
    der_list chain_der;
    br_x509_certificate chain[4];
    br_skey_decoder_context key;
    /* Bytes on their way to the client. */
    unsigned char out[64 * 1024];
    size_t out_len, out_off;
    /* What the client sent, and the canned answer once its request head is complete. */
    char req[4096];
    size_t req_len;
    const char *response;
    size_t resp_off;
    bool close_after_response;
    bool hang_up; /* close the TCP connection as soon as the client speaks */
    int connects;
    bool open;
    xc_net_conn conn;
} server;

static server S;

static void server_pump(server *s)
{
    br_ssl_engine_context *eng = &s->sc.eng;
    for (;;) {
        unsigned st = br_ssl_engine_current_state(eng);
        if (st & BR_SSL_CLOSED) {
            return;
        }
        if (st & BR_SSL_SENDREC) {
            size_t n = 0;
            unsigned char *buf = br_ssl_engine_sendrec_buf(eng, &n);
            TEST_ASSERT_TRUE(s->out_len + n <= sizeof s->out);
            memcpy(s->out + s->out_len, buf, n);
            s->out_len += n;
            br_ssl_engine_sendrec_ack(eng, n);
            continue;
        }
        if (st & BR_SSL_RECVAPP) {
            size_t n = 0;
            unsigned char *buf = br_ssl_engine_recvapp_buf(eng, &n);
            TEST_ASSERT_TRUE(s->req_len + n < sizeof s->req);
            memcpy(s->req + s->req_len, buf, n);
            s->req_len += n;
            s->req[s->req_len] = '\0';
            br_ssl_engine_recvapp_ack(eng, n);
            continue;
        }
        bool head_done = strstr(s->req, "\r\n\r\n") != NULL;
        if ((st & BR_SSL_SENDAPP) && head_done && s->response != NULL &&
            s->resp_off < strlen(s->response)) {
            size_t room = 0;
            unsigned char *buf = br_ssl_engine_sendapp_buf(eng, &room);
            size_t n = strlen(s->response) - s->resp_off;
            n = n < room ? n : room;
            memcpy(buf, s->response + s->resp_off, n);
            br_ssl_engine_sendapp_ack(eng, n);
            s->resp_off += n;
            br_ssl_engine_flush(eng, 0);
            if (s->resp_off == strlen(s->response) && s->close_after_response) {
                br_ssl_engine_close(eng);
            }
            continue;
        }
        return;
    }
}

static xc_err f_connect(void *ctx, const char *host, uint16_t port, uint32_t timeout_ms,
                        xc_net_conn **out)
{
    server *s = ctx;
    (void)host;
    (void)port;
    (void)timeout_ms;
    TEST_ASSERT_FALSE(s->open);
    s->connects++;
    s->open = true;
    s->out_len = s->out_off = 0;
    s->req_len = 0;
    s->req[0] = '\0';
    s->resp_off = 0;
    TEST_ASSERT_TRUE(br_ssl_server_reset(&s->sc));
    *out = &s->conn;
    return XC_OK;
}

static xc_err f_send(void *ctx, xc_net_conn *conn, const void *buf, size_t len, uint32_t timeout_ms)
{
    server *s = ctx;
    (void)timeout_ms;
    TEST_ASSERT_EQUAL_PTR(&s->conn, conn);
    TEST_ASSERT_TRUE(s->open);
    if (s->hang_up) {
        return XC_OK; /* swallowed; recv then reports the close */
    }
    const unsigned char *p = buf;
    while (len > 0) {
        server_pump(s);
        unsigned st = br_ssl_engine_current_state(&s->sc.eng);
        if (st & BR_SSL_CLOSED) {
            return XC_OK; /* the server gave up; like TCP, the bytes are accepted unread */
        }
        TEST_ASSERT_TRUE(st & BR_SSL_RECVREC);
        size_t room = 0;
        unsigned char *dst = br_ssl_engine_recvrec_buf(&s->sc.eng, &room);
        size_t n = len < room ? len : room;
        memcpy(dst, p, n);
        br_ssl_engine_recvrec_ack(&s->sc.eng, n);
        p += n;
        len -= n;
    }
    server_pump(s);
    return XC_OK;
}

static xc_err f_recv(void *ctx, xc_net_conn *conn, void *buf, size_t cap, uint32_t timeout_ms,
                     size_t *got)
{
    server *s = ctx;
    (void)timeout_ms;
    TEST_ASSERT_EQUAL_PTR(&s->conn, conn);
    *got = 0;
    if (s->hang_up) {
        return XC_OK;
    }
    server_pump(s);
    if (s->out_off < s->out_len) {
        size_t n = s->out_len - s->out_off;
        n = n < cap ? n : cap;
        memcpy(buf, s->out + s->out_off, n);
        s->out_off += n;
        *got = n;
        return XC_OK;
    }
    if (br_ssl_engine_current_state(&s->sc.eng) & BR_SSL_CLOSED) {
        return XC_OK; /* the server closed the TCP connection */
    }
    return XC_ERR_NET_TIMEOUT; /* nothing to say: both sides would wait forever */
}

static void f_close(void *ctx, xc_net_conn *conn)
{
    server *s = ctx;
    TEST_ASSERT_EQUAL_PTR(&s->conn, conn);
    s->open = false;
}

static const xc_net tcp = {f_connect, f_send, f_recv, f_close, &S};

static const unsigned char server_seed[] = "in-process test server seed, not secret";

/* Starts the server with leaf + chain `pem` and private key `key_der`. */
static void server_start(const char *chain_pem, const char *key_der)
{
    memset(&S, 0, sizeof S);
    size_t len = 0;
    char *pem = read_file(chain_pem, &len);
    pem_to_der(pem, len, &S.chain_der);
    free(pem);
    for (size_t i = 0; i < S.chain_der.count; i++) {
        S.chain[i].data = S.chain_der.data[i];
        S.chain[i].data_len = S.chain_der.len[i];
    }
    char *key = read_file(key_der, &len);
    br_skey_decoder_init(&S.key);
    br_skey_decoder_push(&S.key, key, len);
    free(key);
    TEST_ASSERT_EQUAL_INT(0, br_skey_decoder_last_error(&S.key));
    if (br_skey_decoder_key_type(&S.key) == BR_KEYTYPE_EC) {
        br_ssl_server_init_full_ec(&S.sc, S.chain, S.chain_der.count, BR_KEYTYPE_EC,
                                   br_skey_decoder_get_ec(&S.key));
    } else {
        br_ssl_server_init_full_rsa(&S.sc, S.chain, S.chain_der.count,
                                    br_skey_decoder_get_rsa(&S.key));
    }
    br_ssl_engine_set_buffer(&S.sc.eng, S.iobuf, sizeof S.iobuf, 1);
    br_ssl_engine_inject_entropy(&S.sc.eng, server_seed, sizeof server_seed);
    S.response = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
}

/* ---- The client side ------------------------------------------------------------------ */

static uint64_t clock_ms; /* the HTTP client's fake monotonic clock */
static int64_t now_value;
static bool now_fails;
static bool random_fails;

static xc_err w_now(void *ctx, int64_t *unix_s)
{
    (void)ctx;
    if (now_fails) {
        return XC_ERR_TLS_NO_TIME;
    }
    *unix_s = now_value;
    return XC_OK;
}

static xc_err e_get(void *ctx, void *buf, size_t len)
{
    (void)ctx;
    if (random_fails) {
        return XC_ERR_TLS_NO_RANDOM;
    }
    /* Fixed bytes are fine in a test; real platforms use real randomness (ADR 0012 §6). */
    memset(buf, 0x5A, len);
    return XC_OK;
}

static const xc_wallclock wallclock = {w_now, NULL};
static const xc_entropy entropy = {e_get, NULL};

static xc_tls_anchors *anchors;
static xc_tls *tls;
static xc_net tls_net;

static void client_start(const char *anchor_file)
{
    size_t len = 0;
    char *pem = read_file(anchor_file, &len);
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_tls_anchors_parse(pem, len, &anchors, NULL));
    free(pem);
    xc_tls_config cfg = {&tcp, anchors, &entropy, &wallclock};
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_tls_create(&cfg, &tls, &tls_net));
}

void setUp(void)
{
    clock_ms = 0;
    now_value = T_NOW;
    now_fails = false;
    random_fails = false;
    anchors = NULL;
    tls = NULL;
    server_start("server-chain.pem", "server.key.der");
}

void tearDown(void)
{
    xc_tls_destroy(tls);
    xc_tls_anchors_free(anchors);
}

static xc_err tls_open(const char *host, xc_net_conn **conn)
{
    return tls_net.connect(tls_net.ctx, host, 443, 1000, conn);
}

/* Sends a request, reads the whole answer into `out`. */
static void exchange(xc_net_conn *conn, char *out, size_t cap)
{
    const char *req = "GET / HTTP/1.1\r\nHost: test.example\r\n\r\n";
    TEST_ASSERT_EQUAL_INT(XC_OK, tls_net.send(tls_net.ctx, conn, req, strlen(req), 1000));
    size_t have = 0;
    const char *want = S.response;
    while (have < strlen(want)) {
        size_t got = 0;
        TEST_ASSERT_EQUAL_INT(
            XC_OK, tls_net.recv(tls_net.ctx, conn, out + have, cap - 1 - have, 1000, &got));
        TEST_ASSERT_TRUE(got > 0);
        have += got;
    }
    out[have] = '\0';
    TEST_ASSERT_EQUAL_STRING(req, S.req);
}

/* ---- Trust anchors ------------------------------------------------------------------- */

static xc_err parse(const char *text, size_t *count, size_t *bad)
{
    xc_tls_anchors *a = NULL;
    xc_err err = xc_tls_anchors_parse(text, strlen(text), &a, bad);
    *count = xc_tls_anchors_count(a);
    xc_tls_anchors_free(a);
    return err;
}

static void test_anchors_from_files(void)
{
    size_t len = 0;
    char *pem = read_file("server-chain.pem", &len);
    xc_tls_anchors *a = NULL;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_tls_anchors_parse(pem, len, &a, NULL));
    TEST_ASSERT_EQUAL_UINT(2, xc_tls_anchors_count(a));
    xc_tls_anchors_free(a);
    /* Without the final newline, and with CRLF line ends. */
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_tls_anchors_parse(pem, len - 1, &a, NULL));
    TEST_ASSERT_EQUAL_UINT(2, xc_tls_anchors_count(a));
    xc_tls_anchors_free(a);
    char *crlf = malloc(2 * len);
    TEST_ASSERT_NOT_NULL(crlf);
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        if (pem[i] == '\n') {
            crlf[n++] = '\r';
        }
        crlf[n++] = pem[i];
    }
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_tls_anchors_parse(crlf, n, &a, NULL));
    TEST_ASSERT_EQUAL_UINT(2, xc_tls_anchors_count(a));
    xc_tls_anchors_free(a);
    free(crlf);
    free(pem);
}

static void test_anchors_text_around_objects_is_ignored(void)
{
    size_t len = 0;
    char *ca = read_file("ca.pem", &len);
    char *text = malloc(len + 200);
    TEST_ASSERT_NOT_NULL(text);
    int n =
        snprintf(text, len + 200,
                 "## curl-style header\n\nxcat test CA\n============\n%.*s\n# end\n", (int)len, ca);
    TEST_ASSERT_TRUE(n > 0);
    size_t count = 0;
    TEST_ASSERT_EQUAL_INT(XC_OK, parse(text, &count, NULL));
    TEST_ASSERT_EQUAL_UINT(1, count);
    free(text);
    free(ca);
}

static void test_anchors_rejected(void)
{
    size_t count = 0;
    size_t bad = 99;
    TEST_ASSERT_EQUAL_INT(XC_ERR_CORRUPT, parse("", &count, &bad));
    TEST_ASSERT_EQUAL_UINT(0, bad);
    TEST_ASSERT_EQUAL_INT(XC_ERR_CORRUPT, parse("just some text\n", &count, &bad));
    /* Not a certificate. */
    TEST_ASSERT_EQUAL_INT(
        XC_ERR_CORRUPT,
        parse("-----BEGIN PRIVATE KEY-----\nAAAA\n-----END PRIVATE KEY-----\n", &count, &bad));
    TEST_ASSERT_EQUAL_UINT(1, bad);
    /* A certificate object holding no certificate. */
    TEST_ASSERT_EQUAL_INT(
        XC_ERR_CORRUPT,
        parse("-----BEGIN CERTIFICATE-----\nAAAA\n-----END CERTIFICATE-----\n", &count, &bad));
    TEST_ASSERT_EQUAL_UINT(1, bad);
    /* Never ends. */
    TEST_ASSERT_EQUAL_INT(XC_ERR_CORRUPT,
                          parse("-----BEGIN CERTIFICATE-----\nAAAA\n", &count, &bad));
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_tls_anchors_parse("x", 1, NULL, NULL));
}

static void test_anchors_second_certificate_bad(void)
{
    size_t len = 0;
    char *ca = read_file("ca.pem", &len);
    const char *junk = "-----BEGIN CERTIFICATE-----\nMIIB\n-----END CERTIFICATE-----\n";
    char *text = malloc(len + strlen(junk) + 1);
    TEST_ASSERT_NOT_NULL(text);
    memcpy(text, ca, len);
    memcpy(text + len, junk, strlen(junk) + 1);
    size_t count = 0;
    size_t bad = 0;
    TEST_ASSERT_EQUAL_INT(XC_ERR_CORRUPT, parse(text, &count, &bad));
    TEST_ASSERT_EQUAL_UINT(0, count); /* all or nothing */
    TEST_ASSERT_EQUAL_UINT(2, bad);
    free(text);
    free(ca);
}

static void test_anchors_limits(void)
{
    size_t len = 0;
    char *ca = read_file("ca.pem", &len);
    size_t total = len * (XC_TLS_MAX_ANCHORS + 1);
    char *text = malloc(total);
    TEST_ASSERT_NOT_NULL(text);
    for (size_t i = 0; i <= XC_TLS_MAX_ANCHORS; i++) {
        memcpy(text + i * len, ca, len);
    }
    xc_tls_anchors *a = NULL;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_tls_anchors_parse(text, total - len, &a, NULL));
    TEST_ASSERT_EQUAL_UINT(XC_TLS_MAX_ANCHORS, xc_tls_anchors_count(a));
    xc_tls_anchors_free(a);
    TEST_ASSERT_EQUAL_INT(XC_ERR_LIMIT, xc_tls_anchors_parse(text, total, &a, NULL));
    TEST_ASSERT_NULL(a);
    TEST_ASSERT_EQUAL_INT(XC_ERR_LIMIT,
                          xc_tls_anchors_parse(text, XC_TLS_MAX_ANCHOR_FILE + 1, &a, NULL));
    free(text);
    free(ca);
}

/* ---- Connections --------------------------------------------------------------------- */

static void test_connect_and_exchange(void)
{
    client_start("ca.pem");
    xc_net_conn *conn = NULL;
    TEST_ASSERT_EQUAL_INT(XC_OK, tls_open("test.example", &conn));
    char out[256];
    exchange(conn, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING(S.response, out);
    xc_tls_info info;
    xc_tls_last_info(tls, &info);
    TEST_ASSERT_EQUAL_HEX(0x0303, info.version);
    TEST_ASSERT_EQUAL_STRING("ECDHE-ECDSA-CHACHA20-POLY1305", info.suite_name);
    tls_net.close(tls_net.ctx, conn);
    TEST_ASSERT_FALSE(S.open);
    /* The same client connects again. */
    TEST_ASSERT_EQUAL_INT(XC_OK, tls_open("localhost", &conn));
    exchange(conn, out, sizeof out);
    tls_net.close(tls_net.ctx, conn);
    TEST_ASSERT_EQUAL_INT(2, S.connects);
}

static void test_rsa_server(void)
{
    server_start("rsa-server.pem", "rsa-server.key.der");
    client_start("rsa-ca.pem");
    xc_net_conn *conn = NULL;
    TEST_ASSERT_EQUAL_INT(XC_OK, tls_open("test.example", &conn));
    xc_tls_info info;
    xc_tls_last_info(tls, &info);
    TEST_ASSERT_EQUAL_STRING("ECDHE-RSA-CHACHA20-POLY1305", info.suite_name);
    char out[256];
    exchange(conn, out, sizeof out);
    tls_net.close(tls_net.ctx, conn);
}

static void test_aes_gcm_when_the_server_prefers_it(void)
{
    static const uint16_t gcm[] = {BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256};
    br_ssl_engine_set_suites(&S.sc.eng, gcm, 1);
    client_start("ca.pem");
    xc_net_conn *conn = NULL;
    TEST_ASSERT_EQUAL_INT(XC_OK, tls_open("test.example", &conn));
    xc_tls_info info;
    xc_tls_last_info(tls, &info);
    TEST_ASSERT_EQUAL_STRING("ECDHE-ECDSA-AES128-GCM-SHA256", info.suite_name);
    char out[256];
    exchange(conn, out, sizeof out);
    tls_net.close(tls_net.ctx, conn);
}

static void expect_refused(const char *host, xc_err want)
{
    xc_net_conn *conn = NULL;
    TEST_ASSERT_EQUAL_INT(want, tls_open(host, &conn));
    TEST_ASSERT_NULL(conn);
    TEST_ASSERT_FALSE(S.open); /* the TCP connection was closed */
}

static void test_unknown_issuer(void)
{
    client_start("other-ca.pem");
    expect_refused("test.example", XC_ERR_TLS_CERT);
    TEST_ASSERT_TRUE(xc_tls_last_error(tls) > BR_ERR_X509_OK);
}

static void test_wrong_host_name(void)
{
    client_start("ca.pem");
    expect_refused("archive.example", XC_ERR_TLS_CERT);
    TEST_ASSERT_EQUAL_INT(BR_ERR_X509_BAD_SERVER_NAME, xc_tls_last_error(tls));
}

static void test_wrong_clock(void)
{
    client_start("ca.pem");
    now_value = T_RESET_CLOCK;
    expect_refused("test.example", XC_ERR_TLS_CERT_TIME);
    now_value = T_EXPIRED;
    expect_refused("test.example", XC_ERR_TLS_CERT_TIME);
    now_value = T_VALID - 1;
    expect_refused("test.example", XC_ERR_TLS_CERT_TIME);
    /* The first second of validity: the day arithmetic is exact. */
    now_value = T_VALID;
    xc_net_conn *conn = NULL;
    TEST_ASSERT_EQUAL_INT(XC_OK, tls_open("test.example", &conn));
    tls_net.close(tls_net.ctx, conn);
}

static void test_no_time_or_randomness_sends_nothing(void)
{
    client_start("ca.pem");
    now_fails = true;
    expect_refused("test.example", XC_ERR_TLS_NO_TIME);
    now_fails = false;
    now_value = -1;
    expect_refused("test.example", XC_ERR_TLS_NO_TIME);
    now_value = T_NOW;
    random_fails = true;
    expect_refused("test.example", XC_ERR_TLS_NO_RANDOM);
    TEST_ASSERT_EQUAL_INT(0, S.connects);
}

static void test_no_common_cipher_or_version(void)
{
    static const uint16_t cbc[] = {BR_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256};
    br_ssl_engine_set_suites(&S.sc.eng, cbc, 1);
    client_start("ca.pem");
    expect_refused("test.example", XC_ERR_TLS);

    server_start("server-chain.pem", "server.key.der");
    br_ssl_engine_set_versions(&S.sc.eng, BR_TLS11, BR_TLS11);
    expect_refused("test.example", XC_ERR_TLS);
}

static void test_server_hangs_up_mid_handshake(void)
{
    client_start("ca.pem");
    S.hang_up = true;
    expect_refused("test.example", XC_ERR_NET_IO);
}

static void test_close_notify_ends_the_stream(void)
{
    S.close_after_response = true;
    client_start("ca.pem");
    xc_net_conn *conn = NULL;
    TEST_ASSERT_EQUAL_INT(XC_OK, tls_open("test.example", &conn));
    char out[256];
    exchange(conn, out, sizeof out);
    size_t got = 1;
    TEST_ASSERT_EQUAL_INT(XC_OK, tls_net.recv(tls_net.ctx, conn, out, sizeof out, 1000, &got));
    TEST_ASSERT_EQUAL_UINT(0, got);
    tls_net.close(tls_net.ctx, conn);
}

static void test_invalid_arguments(void)
{
    client_start("ca.pem");
    xc_net_conn *conn = NULL;
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, tls_open("", &conn));
    TEST_ASSERT_EQUAL_INT(XC_OK, tls_open("test.example", &conn));
    xc_net_conn *second = NULL;
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, tls_open("test.example", &second));
    tls_net.close(tls_net.ctx, conn);

    xc_tls *t = NULL;
    xc_net n;
    xc_tls_config cfg = {&tcp, anchors, &entropy, NULL};
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_tls_create(&cfg, &t, &n));
    cfg.wallclock = &wallclock;
    cfg.anchors = NULL;
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_tls_create(&cfg, &t, &n));
    TEST_ASSERT_NULL(t);
}

/* ---- Through the HTTP client ---------------------------------------------------------- */

static uint64_t c_now(void *ctx)
{
    (void)ctx;
    return clock_ms;
}
static void c_sleep(void *ctx, uint32_t ms)
{
    (void)ctx;
    clock_ms += ms;
}
static const xc_clock fclock = {c_now, c_sleep, NULL};

static uint8_t http_buf[XC_HTTP_MIN_BUF];

static xc_err http_probe(const char *url, xc_http_resource *r)
{
    xc_http_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.net = &tcp; /* never used: every URL here is https */
    cfg.tls = &tls_net;
    cfg.clock = &fclock;
    cfg.buf = http_buf;
    cfg.buf_size = sizeof http_buf;
    xc_http_client c;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_http_client_init(&c, &cfg));
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_http_resource_init(r, url, strlen(url)));
    xc_err err = xc_http_probe(&c, r);
    xc_http_client_close(&c);
    return err;
}

static void test_https_probe(void)
{
    S.response = "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 0-0/1234\r\n"
                 "Content-Length: 1\r\n\r\nX";
    client_start("ca.pem");
    xc_http_resource r;
    TEST_ASSERT_EQUAL_INT(XC_OK, http_probe("https://test.example/a.iso", &r));
    TEST_ASSERT_TRUE(r.size_known);
    TEST_ASSERT_EQUAL_UINT64(1234, r.size);
    TEST_ASSERT_NOT_NULL(strstr(S.req, "Range: bytes=0-0\r\n"));
}

static void test_https_certificate_errors_are_not_retried(void)
{
    client_start("other-ca.pem");
    xc_http_resource r;
    TEST_ASSERT_EQUAL_INT(XC_ERR_TLS_CERT, http_probe("https://test.example/a.iso", &r));
    TEST_ASSERT_EQUAL_INT(1, S.connects);
    TEST_ASSERT_EQUAL_UINT64(0, clock_ms); /* no backoff either */
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        certs_dir = argv[1];
    }
    UNITY_BEGIN();
    RUN_TEST(test_anchors_from_files);
    RUN_TEST(test_anchors_text_around_objects_is_ignored);
    RUN_TEST(test_anchors_rejected);
    RUN_TEST(test_anchors_second_certificate_bad);
    RUN_TEST(test_anchors_limits);
    RUN_TEST(test_connect_and_exchange);
    RUN_TEST(test_rsa_server);
    RUN_TEST(test_aes_gcm_when_the_server_prefers_it);
    RUN_TEST(test_unknown_issuer);
    RUN_TEST(test_wrong_host_name);
    RUN_TEST(test_wrong_clock);
    RUN_TEST(test_no_time_or_randomness_sends_nothing);
    RUN_TEST(test_no_common_cipher_or_version);
    RUN_TEST(test_server_hangs_up_mid_handshake);
    RUN_TEST(test_close_notify_ends_the_stream);
    RUN_TEST(test_invalid_arguments);
    RUN_TEST(test_https_probe);
    RUN_TEST(test_https_certificate_errors_are_not_retried);
    return UNITY_END();
}
