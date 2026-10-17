/* Tests for the SNTP client and the time source (xcat/sntp.h, docs/formats/sntp.md). */
#include "unity.h"
#include "xcat/sntp.h"

#include <stdio.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* 2026-10-17 00:00:00 UTC */
#define T_UNIX 1792195200LL
#define T_NTP (uint32_t)(T_UNIX + 2208988800LL)

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24U);
    p[1] = (uint8_t)(v >> 16U);
    p[2] = (uint8_t)(v >> 8U);
    p[3] = (uint8_t)v;
}

/* A valid answer to `req` (VN 4, stratum 2): received at T, sent at T + 0.5 s. */
static void answer(uint8_t out[48], const uint8_t *req)
{
    memset(out, 0, 48);
    out[0] = 0x24; /* LI 0, VN 4, Mode 4 */
    out[1] = 2;
    memcpy(out + 24, req + 40, 8);
    put32(out + 32, T_NTP);
    put32(out + 36, 0);
    put32(out + 40, T_NTP);
    put32(out + 44, 0x80000000U);
}

static const uint8_t nonce[8] = {1, 2, 3, 4, 5, 6, 7, 8};

static void test_request(void)
{
    uint8_t req[48];
    memset(req, 0xEE, sizeof req);
    xc_sntp_request(req, nonce);
    TEST_ASSERT_EQUAL_HEX8(0x23, req[0]);
    for (size_t i = 1; i < 40; i++) {
        TEST_ASSERT_EQUAL_HEX8(0, req[i]);
    }
    TEST_ASSERT_EQUAL_MEMORY(nonce, req + 40, 8);
}

static xc_sntp_reply r;
static const char *why;
static char kiss[5];

static xc_err parse(const uint8_t *buf, size_t len)
{
    memset(&r, 0, sizeof r);
    return xc_sntp_parse(buf, len, nonce, &r, &why, kiss);
}

static void test_parse_ok(void)
{
    uint8_t req[48];
    uint8_t a[68];
    xc_sntp_request(req, nonce);
    answer(a, req);
    TEST_ASSERT_EQUAL_INT(XC_OK, parse(a, 48));
    TEST_ASSERT_EQUAL_INT64(T_UNIX * 1000 + 500, r.transmit_ms);
    TEST_ASSERT_EQUAL_INT64(T_UNIX * 1000, r.receive_ms);
    TEST_ASSERT_EQUAL_UINT8(2, r.stratum);
    TEST_ASSERT_EQUAL_UINT8(4, r.version);
    TEST_ASSERT_NULL(why);
    TEST_ASSERT_EQUAL_STRING("", kiss);

    /* §4.1: bytes after the 48th are ignored; §4.3: version 3 is accepted. */
    memset(a + 48, 0xFF, 20);
    a[0] = 0x1C; /* LI 0, VN 3, Mode 4 */
    TEST_ASSERT_EQUAL_INT(XC_OK, parse(a, 68));
    TEST_ASSERT_EQUAL_UINT8(3, r.version);

    /* §3: the fraction is rounded down to milliseconds; a zero receive time is "not set". */
    put32(a + 44, 0xFFFFFFFFU);
    memset(a + 32, 0, 8);
    TEST_ASSERT_EQUAL_INT(XC_OK, parse(a, 48));
    TEST_ASSERT_EQUAL_INT64(T_UNIX * 1000 + 999, r.transmit_ms);
    TEST_ASSERT_EQUAL_INT64(0, r.receive_ms);
}

static void test_era(void)
{
    uint8_t req[48];
    uint8_t a[48];
    xc_sntp_request(req, nonce);
    answer(a, req);
    /* 2100-01-01 00:00:00 UTC is in era 1: NTP seconds wrapped, top bit 0 (§3). */
    put32(a + 40, (uint32_t)(4102444800LL + 2208988800LL - 4294967296LL));
    put32(a + 44, 0);
    TEST_ASSERT_EQUAL_INT(XC_OK, parse(a, 48));
    TEST_ASSERT_EQUAL_INT64(4102444800LL * 1000, r.transmit_ms);
    /* 1968-01-20, the earliest era-0 time the top bit allows. */
    put32(a + 40, 0x80000000U);
    TEST_ASSERT_EQUAL_INT(XC_OK, parse(a, 48));
    TEST_ASSERT_EQUAL_INT64((2147483648LL - 2208988800LL) * 1000, r.transmit_ms);
}

static void expect_refused(const uint8_t *a, size_t len, const char *reason)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(XC_ERR_NTP, parse(a, len), reason);
    TEST_ASSERT_NOT_NULL(why);
    TEST_ASSERT_EQUAL_STRING(reason, why);
}

static void test_parse_refused(void)
{
    uint8_t req[48];
    uint8_t good[48];
    uint8_t a[48];
    xc_sntp_request(req, nonce);
    answer(good, req);

    expect_refused(good, 47, "answer too short");
    expect_refused(good, 0, "answer too short");

    memcpy(a, good, 48);
    a[0] = 0x23; /* Mode 3: our own request echoed */
    expect_refused(a, 48, "not a server answer");
    a[0] = 0x25; /* Mode 5: broadcast */
    expect_refused(a, 48, "not a server answer");
    a[0] = 0x14; /* VN 2 */
    expect_refused(a, 48, "unknown NTP version");
    a[0] = 0x3C; /* VN 7 */
    expect_refused(a, 48, "unknown NTP version");
    a[0] = 0xE4; /* LI 3 */
    expect_refused(a, 48, "server not synchronized");

    memcpy(a, good, 48);
    a[1] = 16;
    expect_refused(a, 48, "server not synchronized");
    a[1] = 255;
    expect_refused(a, 48, "server not synchronized");

    memcpy(a, good, 48);
    a[31] ^= 1U;
    expect_refused(a, 48, "answer to a different request");

    memcpy(a, good, 48);
    memset(a + 40, 0, 8);
    expect_refused(a, 48, "no transmit time");

    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_sntp_parse(NULL, 48, nonce, &r, &why, NULL));
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_sntp_parse(good, 48, nonce, &r, NULL, NULL));
}

static void test_kiss(void)
{
    uint8_t req[48];
    uint8_t a[48];
    xc_sntp_request(req, nonce);
    answer(a, req);
    a[1] = 0;
    memcpy(a + 12, "RATE", 4);
    expect_refused(a, 48, "kiss-o'-death");
    TEST_ASSERT_EQUAL_STRING("RATE", kiss);
    a[13] = 0x01; /* not printable: shown as '?' */
    expect_refused(a, 48, "kiss-o'-death");
    TEST_ASSERT_EQUAL_STRING("R?TE", kiss);
    TEST_ASSERT_EQUAL_INT(XC_ERR_NTP, xc_sntp_parse(a, 48, nonce, &r, &why, NULL));
}

static void test_arrival(void)
{
    xc_sntp_reply x = {10000, 9980, 2, 4}; /* server took 20 ms */
    TEST_ASSERT_EQUAL_INT64(10040, xc_sntp_arrival_ms(&x, 100));
    TEST_ASSERT_EQUAL_INT64(10000, xc_sntp_arrival_ms(&x, 20));
    TEST_ASSERT_EQUAL_INT64(10000, xc_sntp_arrival_ms(&x, 5)); /* never negative */
    x.receive_ms = 0;                                          /* not set: server time 0 */
    TEST_ASSERT_EQUAL_INT64(10050, xc_sntp_arrival_ms(&x, 100));
    x.receive_ms = 10500; /* after transmit: ignored */
    TEST_ASSERT_EQUAL_INT64(10050, xc_sntp_arrival_ms(&x, 100));
}

/* --- queries over a scripted fake --------------------------------------------------------- */

typedef enum step { GOOD, TIMEOUT, DNS, IO, KISS, WRONG_NONCE, EARLY } step;

typedef struct fake {
    step steps[4];
    unsigned calls;
    uint64_t mono;
    uint8_t counter;
    char host[32];
    uint16_t port;
    uint32_t timeout;
} fake;

static fake f;

static xc_err fake_exchange(void *ctx, const char *host, uint16_t port, const void *req,
                            size_t req_len, void *resp, size_t cap, uint32_t timeout_ms,
                            size_t *got, uint32_t *rtt_ms)
{
    fake *k = ctx;
    TEST_ASSERT_EQUAL_size_t(48, req_len);
    TEST_ASSERT_TRUE(cap >= 48);
    (void)snprintf(k->host, sizeof k->host, "%s", host);
    k->port = port;
    k->timeout = timeout_ms;
    step s = k->steps[k->calls++];
    k->mono += 100;
    *rtt_ms = 100;
    uint8_t *a = resp;
    switch (s) {
    case TIMEOUT:
        return XC_ERR_NET_TIMEOUT;
    case DNS:
        return XC_ERR_NET_DNS;
    case IO:
        return XC_ERR_NET_IO;
    default:
        break;
    }
    answer(a, req);
    if (s == KISS) {
        a[1] = 0;
        memcpy(a + 12, "DENY", 4);
    } else if (s == WRONG_NONCE) {
        a[24] ^= 0xFFU;
    } else if (s == EARLY) {
        put32(a + 40, (uint32_t)(1000000000LL + 2208988800LL)); /* 2001 */
    }
    *got = 48;
    return XC_OK;
}

static uint64_t fake_now(void *ctx)
{
    return ((fake *)ctx)->mono;
}

static xc_err fake_random(void *ctx, void *buf, size_t len)
{
    fake *k = ctx;
    uint8_t *p = buf;
    for (size_t i = 0; i < len; i++) {
        p[i] = k->counter++;
    }
    return XC_OK;
}

static xc_err no_random(void *ctx, void *buf, size_t len)
{
    (void)ctx;
    (void)buf;
    (void)len;
    return XC_ERR_TLS_NO_RANDOM;
}

static xc_udp udp = {fake_exchange, &f};
static xc_clock clk = {fake_now, NULL, &f};
static xc_entropy ent = {fake_random, &f};

static xc_sntp_result res;
static xc_sntp_failure fail;

static xc_err query(step a, step b, step c)
{
    memset(&f, 0, sizeof f);
    f.mono = 5000;
    f.steps[0] = a;
    f.steps[1] = b;
    f.steps[2] = c;
    memset(&res, 0, sizeof res);
    memset(&fail, 0x55, sizeof fail);
    return xc_sntp_query(&udp, &clk, &ent, "pool.ntp.org", 123, 2000, T_UNIX - 86400, &res, &fail);
}

static void test_query(void)
{
    TEST_ASSERT_EQUAL_INT(XC_OK, query(GOOD, GOOD, GOOD));
    TEST_ASSERT_EQUAL_UINT(1, f.calls);
    TEST_ASSERT_EQUAL_STRING("pool.ntp.org", f.host);
    TEST_ASSERT_EQUAL_UINT16(123, f.port);
    TEST_ASSERT_EQUAL_UINT32(2000, f.timeout);
    /* transmit T + 500 ms, server took 500 ms of the 100 ms round trip: no correction. */
    TEST_ASSERT_EQUAL_INT64(T_UNIX * 1000 + 500, res.unix_ms);
    TEST_ASSERT_EQUAL_UINT64(5100, res.mono_ms);
    TEST_ASSERT_EQUAL_UINT32(100, res.rtt_ms);
    TEST_ASSERT_EQUAL_UINT8(2, res.stratum);
    TEST_ASSERT_EQUAL_UINT(1, res.attempts);

    /* §6: lost datagrams and stray answers cost an attempt each. */
    TEST_ASSERT_EQUAL_INT(XC_OK, query(TIMEOUT, WRONG_NONCE, GOOD));
    TEST_ASSERT_EQUAL_UINT(3, f.calls);
    TEST_ASSERT_EQUAL_UINT(3, res.attempts);
    TEST_ASSERT_EQUAL_INT(XC_OK, query(IO, GOOD, GOOD));
    TEST_ASSERT_EQUAL_UINT(2, res.attempts);
}

static void test_query_fails(void)
{
    TEST_ASSERT_EQUAL_INT(XC_ERR_NET_TIMEOUT, query(TIMEOUT, TIMEOUT, TIMEOUT));
    TEST_ASSERT_EQUAL_UINT(3, f.calls);
    TEST_ASSERT_EQUAL_INT(XC_ERR_NET_TIMEOUT, fail.err);
    TEST_ASSERT_EQUAL_UINT(3, fail.attempts);
    TEST_ASSERT_NULL(fail.why);

    TEST_ASSERT_EQUAL_INT(XC_ERR_NTP, query(WRONG_NONCE, WRONG_NONCE, WRONG_NONCE));
    TEST_ASSERT_EQUAL_STRING("answer to a different request", fail.why);
    TEST_ASSERT_EQUAL_UINT(3, fail.attempts);

    /* These end the query at once. */
    TEST_ASSERT_EQUAL_INT(XC_ERR_NET_DNS, query(DNS, GOOD, GOOD));
    TEST_ASSERT_EQUAL_UINT(1, f.calls);
    TEST_ASSERT_EQUAL_INT(XC_ERR_NTP, query(KISS, GOOD, GOOD));
    TEST_ASSERT_EQUAL_UINT(1, f.calls);
    TEST_ASSERT_EQUAL_STRING("DENY", fail.kiss);
    TEST_ASSERT_EQUAL_STRING("kiss-o'-death", fail.why);
    TEST_ASSERT_EQUAL_INT(XC_ERR_NTP, query(EARLY, GOOD, GOOD)); /* §4.8 */
    TEST_ASSERT_EQUAL_UINT(1, f.calls);
    TEST_ASSERT_EQUAL_STRING("time earlier than this program's build", fail.why);
    TEST_ASSERT_EQUAL_STRING("", fail.kiss);

    xc_entropy none = {no_random, NULL};
    memset(&f, 0, sizeof f);
    TEST_ASSERT_EQUAL_INT(XC_ERR_TLS_NO_RANDOM,
                          xc_sntp_query(&udp, &clk, &none, "h", 123, 1, 0, &res, &fail));
    TEST_ASSERT_EQUAL_UINT(0, f.calls);
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG,
                          xc_sntp_query(&udp, &clk, &ent, NULL, 123, 1, 0, &res, NULL));
}

/* --- time source --------------------------------------------------------------------------- */

static int64_t console_now;

static xc_err console(void *ctx, int64_t *unix_s)
{
    (void)ctx;
    if (console_now == 0) {
        return XC_ERR_TLS_NO_TIME; /* earlier than the build: refused by the platform */
    }
    *unix_s = console_now;
    return XC_OK;
}

static const xc_wallclock console_clock = {console, NULL};

static void test_choose(void)
{
    xc_time_source t;
    xc_wallclock w;
    int64_t now = 0;
    const int64_t floor = T_UNIX - 86400;

    /* NTP answers: the wallclock follows the monotonic clock from that moment. */
    memset(&f, 0, sizeof f);
    f.mono = 5000;
    console_now = T_UNIX - 3600;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_time_choose(&t, &udp, &clk, &ent, "pool.ntp.org", 123, 2000,
                                                &console_clock, floor));
    TEST_ASSERT_EQUAL_INT(XC_TIME_NTP, t.kind);
    TEST_ASSERT_TRUE(t.ntp_asked);
    xc_time_wallclock(&t, &w);
    TEST_ASSERT_EQUAL_INT(XC_OK, w.now(w.ctx, &now));
    TEST_ASSERT_EQUAL_INT64(T_UNIX, now);
    f.mono += 2600; /* 2.6 s later: T + 0.5 + 2.6 */
    TEST_ASSERT_EQUAL_INT(XC_OK, w.now(w.ctx, &now));
    TEST_ASSERT_EQUAL_INT64(T_UNIX + 3, now);

    /* NTP fails: the console's clock, since it isn't below the floor. */
    memset(&f, 0, sizeof f);
    f.steps[0] = DNS;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_time_choose(&t, &udp, &clk, &ent, "pool.ntp.org", 123, 2000,
                                                &console_clock, floor));
    TEST_ASSERT_EQUAL_INT(XC_TIME_FALLBACK, t.kind);
    TEST_ASSERT_EQUAL_INT(XC_ERR_NET_DNS, t.failure.err);
    xc_time_wallclock(&t, &w);
    TEST_ASSERT_EQUAL_INT(XC_OK, w.now(w.ctx, &now));
    TEST_ASSERT_EQUAL_INT64(T_UNIX - 3600, now);

    /* NTP turned off. */
    memset(&f, 0, sizeof f);
    TEST_ASSERT_EQUAL_INT(
        XC_OK, xc_time_choose(&t, &udp, &clk, &ent, NULL, 123, 2000, &console_clock, floor));
    TEST_ASSERT_EQUAL_INT(XC_TIME_FALLBACK, t.kind);
    TEST_ASSERT_FALSE(t.ntp_asked);
    TEST_ASSERT_EQUAL_UINT(0, f.calls);

    /* Neither: the console's clock is below the floor, or refused by the platform. */
    console_now = floor - 1;
    memset(&f, 0, sizeof f);
    f.steps[0] = DNS;
    TEST_ASSERT_EQUAL_INT(XC_ERR_TLS_NO_TIME, xc_time_choose(&t, &udp, &clk, &ent, "pool.ntp.org",
                                                             123, 2000, &console_clock, floor));
    TEST_ASSERT_EQUAL_INT(XC_TIME_NONE, t.kind);
    xc_time_wallclock(&t, &w);
    TEST_ASSERT_EQUAL_INT(XC_ERR_TLS_NO_TIME, w.now(w.ctx, &now));
    console_now = 0;
    TEST_ASSERT_EQUAL_INT(XC_ERR_TLS_NO_TIME, xc_time_choose(&t, &udp, &clk, &ent, NULL, 123, 2000,
                                                             &console_clock, floor));
    TEST_ASSERT_EQUAL_INT(XC_ERR_TLS_NO_TIME,
                          xc_time_choose(&t, &udp, &clk, &ent, NULL, 123, 2000, NULL, floor));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_request);
    RUN_TEST(test_parse_ok);
    RUN_TEST(test_era);
    RUN_TEST(test_parse_refused);
    RUN_TEST(test_kiss);
    RUN_TEST(test_arrival);
    RUN_TEST(test_query);
    RUN_TEST(test_query_fails);
    RUN_TEST(test_choose);
    return UNITY_END();
}
