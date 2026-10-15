/*
 * libFuzzer target for the HTTP response parser, the chunked decoder and the
 * URL parser (docs/formats/http.md §2, §4, §5). Every input goes through all
 * three:
 *
 *   - as a response head (xc_http_head_end, then xc_http_parse_head);
 *   - as a chunked body, fed whole and then in 1- and 7-byte pieces, which must
 *     decode identically;
 *   - as "<base URL>\n<Location value>": the first line is parsed as a URL, and
 *     the rest is resolved against it.
 *
 * Results are checked against the documented guarantees; a violation aborts.
 * Seeds are in core/tests/fuzz-seeds/http/. Built like fuzz_xdvdfs.c: a libFuzzer
 * binary with XCAT_FUZZ, and fuzz_http_replay everywhere.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "xcat/http.h"
#include "xcat/url.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#define MAX_BODY 65536

static void check(int ok)
{
    if (!ok) {
        abort();
    }
}

static void fuzz_head(const uint8_t *data, size_t size)
{
    size_t end = xc_http_head_end(data, size);
    check(end <= size);
    if (end == 0) {
        return;
    }
    xc_http_response r;
    if (xc_http_parse_head(data, end, &r) != XC_OK) {
        return;
    }
    check(r.status >= 200 && r.status <= 599);
    check(!(r.chunked && r.has_length));
    check(!r.has_range || r.range_first <= r.range_last);
    check(!(r.has_range && r.has_total) || r.range_last < r.range_total);
    check(r.location == NULL || ((const uint8_t *)r.location >= data &&
                                 (const uint8_t *)r.location + r.location_len <= data + end));
    check(strlen(r.etag) <= XC_HTTP_MAX_VALIDATOR &&
          strlen(r.last_modified) <= XC_HTTP_MAX_VALIDATOR);
}

/* Decodes data in pieces of `step`; returns the decoded length, or SIZE_MAX on error. */
static size_t decode(const uint8_t *data, size_t size, size_t step, uint8_t *out, bool *done)
{
    xc_http_chunked c;
    xc_http_chunked_init(&c);
    size_t pos = 0;
    size_t n = 0;
    while (pos < size && !xc_http_chunked_done(&c)) {
        size_t avail = size - pos < step ? size - pos : step;
        size_t used = 0;
        const uint8_t *d = NULL;
        size_t dl = 0;
        if (xc_http_chunked_feed(&c, data + pos, avail, &used, &d, &dl) != XC_OK) {
            return SIZE_MAX;
        }
        check(used <= avail);
        if (dl > 0) {
            check(d >= data + pos && d + dl <= data + pos + used);
            if (n + dl <= MAX_BODY) {
                memcpy(out + n, d, dl);
            }
            n += dl;
        } else if (used == 0) {
            break;
        }
        pos += used;
    }
    *done = xc_http_chunked_done(&c);
    return n;
}

static uint8_t body_a[MAX_BODY];
static uint8_t body_b[MAX_BODY];

static void fuzz_chunked(const uint8_t *data, size_t size)
{
    bool done_a = false;
    size_t a = decode(data, size, size == 0 ? 1 : size, body_a, &done_a);
    static const size_t steps[] = {1, 7};
    for (size_t i = 0; i < sizeof steps / sizeof steps[0]; i++) {
        bool done_b = false;
        size_t b = decode(data, size, steps[i], body_b, &done_b);
        check(a == b && done_a == done_b);
        if (a != SIZE_MAX && a <= MAX_BODY) {
            check(memcmp(body_a, body_b, a) == 0);
        }
    }
}

static int target_ok(const xc_url *u)
{
    size_t n = strlen(u->target);
    if (n == 0 || n > XC_URL_MAX_TARGET || u->target[0] != '/') {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)u->target[i];
        if (c <= 0x20U || c >= 0x7FU || c == '#') {
            return 0;
        }
    }
    return u->host[0] != '\0' && u->port != 0;
}

static xc_url base;
static xc_url out;

static void fuzz_url(const uint8_t *data, size_t size)
{
    size_t nl = 0;
    while (nl < size && data[nl] != '\n') {
        nl++;
    }
    if (xc_url_parse((const char *)data, nl, &base) != XC_OK) {
        return;
    }
    check(target_ok(&base));
    if (nl < size &&
        xc_url_resolve(&base, (const char *)data + nl + 1, size - nl - 1, &out) == XC_OK) {
        check(target_ok(&out));
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    fuzz_head(data, size);
    fuzz_chunked(data, size);
    fuzz_url(data, size);
    return 0;
}
