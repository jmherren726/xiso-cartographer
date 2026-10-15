/* Tests for HTTP response parsing (xcat/http.h, docs/formats/http.md §4, §5). */
#include "unity.h"
#include "xcat/http.h"

#include <stdio.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static xc_http_response r;

static xc_err head(const char *s)
{
    size_t len = strlen(s);
    size_t end = xc_http_head_end((const uint8_t *)s, len);
    if (end != len) {
        return XC_ERR_INVALID_ARG; /* the test text must be exactly one head */
    }
    return xc_http_parse_head((const uint8_t *)s, len, &r);
}

static void test_head_end(void)
{
    const uint8_t *a = (const uint8_t *)"HTTP/1.1 200 OK\r\nA: b\r\n\r\nBODY";
    TEST_ASSERT_EQUAL_size_t(25, xc_http_head_end(a, 30));
    TEST_ASSERT_EQUAL_size_t(25, xc_http_head_end(a, 25));
    TEST_ASSERT_EQUAL_size_t(0, xc_http_head_end(a, 24));
    const uint8_t *b = (const uint8_t *)"HTTP/1.0 200 OK\nA: b\n\nx";
    TEST_ASSERT_EQUAL_size_t(22, xc_http_head_end(b, 23));
    TEST_ASSERT_EQUAL_size_t(0, xc_http_head_end(b, 0));
}

static void test_partial_content(void)
{
    TEST_ASSERT_EQUAL_INT(XC_OK, head("HTTP/1.1 206 Partial Content\r\n"
                                      "Content-Type: application/octet-stream\r\n"
                                      "content-length: 1000\r\n"
                                      "Content-Range: bytes 1000-1999/4294967296000\r\n"
                                      "ETag: \"abc-123\"\r\n"
                                      "Last-Modified: Tue, 13 Oct 2026 10:00:00 GMT\r\n"
                                      "\r\n"));
    TEST_ASSERT_EQUAL_INT(206, r.status);
    TEST_ASSERT_TRUE(r.has_length);
    TEST_ASSERT_EQUAL_UINT64(1000, r.length);
    TEST_ASSERT_FALSE(r.chunked);
    TEST_ASSERT_TRUE(r.has_range);
    TEST_ASSERT_EQUAL_UINT64(1000, r.range_first);
    TEST_ASSERT_EQUAL_UINT64(1999, r.range_last);
    TEST_ASSERT_TRUE(r.has_total);
    TEST_ASSERT_EQUAL_UINT64(4294967296000ULL, r.range_total);
    TEST_ASSERT_FALSE(r.range_unsatisfied);
    TEST_ASSERT_FALSE(r.close);
    TEST_ASSERT_NULL(r.location);
    TEST_ASSERT_EQUAL_STRING("\"abc-123\"", r.etag);
    TEST_ASSERT_EQUAL_STRING("Tue, 13 Oct 2026 10:00:00 GMT", r.last_modified);
}

static void test_other_headers(void)
{
    TEST_ASSERT_EQUAL_INT(XC_OK, head("HTTP/1.1 302 Found\r\n"
                                      "Location:   https://dn1.example.org/x y  \r\n"
                                      "Connection: keep-alive, Close\r\n"
                                      "Retry-After: 120\r\n"
                                      "Transfer-Encoding: Chunked\r\n"
                                      "X-Odd: caf\xC3\xA9\t!\r\n"
                                      "\r\n"));
    TEST_ASSERT_EQUAL_INT(302, r.status);
    TEST_ASSERT_EQUAL_size_t(27, r.location_len);
    TEST_ASSERT_EQUAL_MEMORY("https://dn1.example.org/x y", r.location, 27);
    TEST_ASSERT_TRUE(r.close);
    TEST_ASSERT_TRUE(r.has_retry_after);
    TEST_ASSERT_EQUAL_UINT32(120, r.retry_after_s);
    TEST_ASSERT_TRUE(r.chunked);

    TEST_ASSERT_EQUAL_INT(XC_OK, head("HTTP/1.1 503 x\r\nRetry-After: Fri, 31 Dec 1999 23:59:59 "
                                      "GMT\r\n\r\n"));
    TEST_ASSERT_FALSE(r.has_retry_after);
    TEST_ASSERT_EQUAL_INT(XC_OK, head("HTTP/1.1 503 x\r\nRetry-After: 9999999999\r\n\r\n"));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, r.retry_after_s);

    TEST_ASSERT_EQUAL_INT(XC_OK, head("HTTP/1.1 416 No\r\nContent-Range: bytes */500\r\n\r\n"));
    TEST_ASSERT_TRUE(r.range_unsatisfied);
    TEST_ASSERT_FALSE(r.has_range);
    TEST_ASSERT_EQUAL_UINT64(500, r.range_total);

    TEST_ASSERT_EQUAL_INT(XC_OK, head("HTTP/1.1 206 x\r\nContent-Range: bytes 0-0/*\r\n\r\n"));
    TEST_ASSERT_TRUE(r.has_range);
    TEST_ASSERT_FALSE(r.has_total);

    /* Status line without a reason, LF line ends, equal repeated lengths, identity coding. */
    TEST_ASSERT_EQUAL_INT(XC_OK, head("HTTP/1.0 200\nContent-Length: 5, 5\ncontent-length: 5\n"
                                      "Content-Encoding: identity\n\n"));
    TEST_ASSERT_EQUAL_UINT64(5, r.length);

    char long_etag[200];
    (void)snprintf(long_etag, sizeof long_etag, "HTTP/1.1 200 OK\r\nETag: \"%0140d\"\r\n\r\n", 0);
    TEST_ASSERT_EQUAL_INT(XC_OK, head(long_etag));
    TEST_ASSERT_EQUAL_STRING("", r.etag); /* too long to keep */
}

static void test_rejected_heads(void)
{
    static const char *const bad[] = {
        "HTTP/1.1 100 Continue\r\n\r\n",
        "HTTP/1.1 600 x\r\n\r\n",
        "HTTP/2 200 OK\r\n\r\n",
        "HTTP/1.1 20 OK\r\n\r\n",
        "HTTP/1.1  200 OK\r\n\r\n",
        "HTTP/1.1 200OK\r\n\r\n",
        "http/1.1 200 OK\r\n\r\n",
        "\r\n\r\n",
        "HTTP/1.1 200 O\x01K\r\n\r\n",
        "HTTP/1.1 200 OK\r\nNo colon\r\n\r\n",
        "HTTP/1.1 200 OK\r\n: empty name\r\n\r\n",
        "HTTP/1.1 200 OK\r\nName : space before colon\r\n\r\n",
        "HTTP/1.1 200 OK\r\nA: b\r\n folded\r\n\r\n",
        "HTTP/1.1 200 OK\r\nA: b\rc\r\n\r\n",
        "HTTP/1.1 200 OK\r\nA: b\x7f\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 5, 6\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: \r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 12345678901234567890\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 0x10\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 5\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\n\r\n",
        "HTTP/1.1 206 x\r\nContent-Range: bytes 5-4/10\r\n\r\n",
        "HTTP/1.1 206 x\r\nContent-Range: bytes 0-10/10\r\n\r\n",
        "HTTP/1.1 206 x\r\nContent-Range: bytes 0-1\r\n\r\n",
        "HTTP/1.1 206 x\r\nContent-Range: items 0-1/2\r\n\r\n",
        "HTTP/1.1 206 x\r\nContent-Range: bytes -1/2\r\n\r\n",
        "HTTP/1.1 206 x\r\nContent-Range: bytes 0-1/2\r\nContent-Range: bytes 0-1/2\r\n\r\n",
        "HTTP/1.1 302 x\r\nLocation: /a\r\nLocation: /b\r\n\r\n",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(XC_ERR_HTTP_PROTOCOL, head(bad[i]), bad[i]);
    }
    /* Text after the blank line isn't part of the head. */
    const char *two = "HTTP/1.1 200 OK\r\n\r\nX: y\r\n\r\n";
    TEST_ASSERT_EQUAL_INT(XC_ERR_HTTP_PROTOCOL,
                          xc_http_parse_head((const uint8_t *)two, strlen(two), &r));
    TEST_ASSERT_EQUAL_INT(XC_ERR_HTTP_PROTOCOL,
                          xc_http_parse_head((const uint8_t *)"HTTP/1.1 200 OK\r\n", 17, &r));
}

static void test_header_limits(void)
{
    static char buf[XC_HTTP_MAX_HEAD + 64];
    size_t n = (size_t)snprintf(buf, sizeof buf, "HTTP/1.1 200 OK\r\n");
    for (int i = 0; i < XC_HTTP_MAX_HEADERS; i++) {
        n += (size_t)snprintf(buf + n, sizeof buf - n, "X-%d: y\r\n", i);
    }
    (void)snprintf(buf + n, sizeof buf - n, "\r\n");
    TEST_ASSERT_EQUAL_INT(XC_OK, head(buf));
    (void)snprintf(buf + n, sizeof buf - n, "X: y\r\n\r\n");
    TEST_ASSERT_EQUAL_INT(XC_ERR_HTTP_PROTOCOL, head(buf));

    n = (size_t)snprintf(buf, sizeof buf, "HTTP/1.1 200 OK\r\nX: ");
    memset(buf + n, 'a', XC_HTTP_MAX_HEAD - n - 4);
    memcpy(buf + XC_HTTP_MAX_HEAD - 4, "\r\n\r\n", 5);
    TEST_ASSERT_EQUAL_INT(XC_OK, head(buf));
    memset(buf + n, 'a', XC_HTTP_MAX_HEAD - n - 3);
    memcpy(buf + XC_HTTP_MAX_HEAD - 3, "\r\n\r\n", 5);
    TEST_ASSERT_EQUAL_INT(XC_ERR_HTTP_PROTOCOL, head(buf));
}

/* Decodes `in`, feeding it `step` bytes at a time; returns the error or XC_OK. */
static xc_err decode(const char *in, size_t len, size_t step, char *out, size_t *out_len,
                     size_t *consumed)
{
    xc_http_chunked c;
    xc_http_chunked_init(&c);
    *out_len = 0;
    size_t pos = 0;
    while (pos < len && !xc_http_chunked_done(&c)) {
        size_t avail = len - pos < step ? len - pos : step;
        size_t used = 0;
        const uint8_t *data = NULL;
        size_t data_len = 0;
        xc_err err =
            xc_http_chunked_feed(&c, (const uint8_t *)in + pos, avail, &used, &data, &data_len);
        if (err != XC_OK) {
            return err;
        }
        TEST_ASSERT_TRUE(used <= avail);
        if (data_len > 0) {
            TEST_ASSERT_TRUE(data >= (const uint8_t *)in + pos);
            memcpy(out + *out_len, data, data_len);
            *out_len += data_len;
        } else if (used == 0) {
            break; /* done */
        }
        pos += used;
    }
    *consumed = pos;
    return xc_http_chunked_done(&c) ? XC_OK : XC_ERR_IO;
}

static void test_chunked(void)
{
    static const char body[] = "5\r\nhello\r\n1;ext=\"v\"\r\n \r\nA  ;x\r\n0123456789\r\n"
                               "0\r\nTrailer: x\r\n\r\nNEXT";
    size_t len = sizeof body - 1;
    for (size_t step = 1; step <= len; step++) {
        char out[64];
        size_t out_len = 0;
        size_t consumed = 0;
        TEST_ASSERT_EQUAL_INT(XC_OK, decode(body, len, step, out, &out_len, &consumed));
        TEST_ASSERT_EQUAL_size_t(16, out_len);
        TEST_ASSERT_EQUAL_MEMORY("hello 0123456789", out, 16);
        TEST_ASSERT_EQUAL_size_t(len - 4, consumed); /* "NEXT" is not part of the body */
    }

    char out[64];
    size_t out_len = 0;
    size_t consumed = 0;
    static const char lf[] = "3\nabc\n0\n\n";
    TEST_ASSERT_EQUAL_INT(XC_OK, decode(lf, sizeof lf - 1, 4, out, &out_len, &consumed));
    TEST_ASSERT_EQUAL_MEMORY("abc", out, 3);
    static const char big[] = "7FFFFFFFFFFFFFFF\r\n";
    TEST_ASSERT_EQUAL_INT(XC_ERR_IO, decode(big, sizeof big - 1, 64, out, &out_len, &consumed));
}

static void test_chunked_rejected(void)
{
    static const char *const bad[] = {
        "\r\n",  /* no size */
        "g\r\n", /* not hex */
        "-1\r\n",
        ";x\r\n",
        "5 x\r\nhello\r\n0\r\n\r\n",
        "5\r\nhelloX\r\n", /* no CRLF after data */
        "5\r\nhello\rX",
        "5\r\x01",
        "8000000000000000\r\n",  /* > 2^63 - 1 */
        "00000000000000001\r\n", /* 17 digits */
        "0\r\nTrailer\x01\r\n\r\n",
        "0\r\n\rX",
        "1;\x01\r\n",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        char out[64];
        size_t out_len = 0;
        size_t consumed = 0;
        TEST_ASSERT_EQUAL_INT_MESSAGE(XC_ERR_HTTP_PROTOCOL,
                                      decode(bad[i], strlen(bad[i]), 1, out, &out_len, &consumed),
                                      bad[i]);
    }

    /* Over-long size line and trailer section. */
    static char lng[XC_HTTP_MAX_TRAILERS + 64];
    memset(lng, 0, sizeof lng);
    lng[0] = '1';
    lng[1] = ';';
    memset(lng + 2, 'e', XC_HTTP_MAX_LINE);
    char out[64];
    size_t out_len = 0;
    size_t consumed = 0;
    TEST_ASSERT_EQUAL_INT(XC_ERR_HTTP_PROTOCOL,
                          decode(lng, strlen(lng), 100, out, &out_len, &consumed));
    size_t n = (size_t)snprintf(lng, sizeof lng, "0\r\n");
    while (n < XC_HTTP_MAX_TRAILERS + 20) {
        n += (size_t)snprintf(lng + n, sizeof lng - n, "X: 0123456789\r\n");
    }
    TEST_ASSERT_EQUAL_INT(XC_ERR_HTTP_PROTOCOL,
                          decode(lng, strlen(lng), 100, out, &out_len, &consumed));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_head_end);
    RUN_TEST(test_partial_content);
    RUN_TEST(test_other_headers);
    RUN_TEST(test_rejected_heads);
    RUN_TEST(test_header_limits);
    RUN_TEST(test_chunked);
    RUN_TEST(test_chunked_rejected);
    return UNITY_END();
}
