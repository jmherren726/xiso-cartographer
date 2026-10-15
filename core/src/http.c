/* HTTP/1.1 response parsing: docs/formats/http.md §4 (head) and §5 (chunked body). */
#include "xcat/http.h"

#include <string.h>

static bool is_digit(uint8_t c)
{
    return c >= '0' && c <= '9';
}

static uint8_t lower(uint8_t c)
{
    return (c >= 'A' && c <= 'Z') ? (uint8_t)(c - 'A' + 'a') : c;
}

/* tchar, RFC 9110 §5.6.2. */
static bool is_tchar(uint8_t c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || is_digit(c)) {
        return true;
    }
    return c != 0 && strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

static bool is_ws(uint8_t c)
{
    return c == ' ' || c == '\t';
}

/* §4.2: no control characters except HT. */
static bool is_value_byte(uint8_t c)
{
    return c == '\t' || (c >= 0x20U && c != 0x7FU);
}

size_t xc_http_head_end(const uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (buf[i] != '\n') {
            continue;
        }
        if (i + 1 < len && buf[i + 1] == '\n') {
            return i + 2;
        }
        if (i + 2 < len && buf[i + 1] == '\r' && buf[i + 2] == '\n') {
            return i + 3;
        }
    }
    return 0;
}

/* A slice of the head: one line or value. */
typedef struct span {
    const uint8_t *p;
    size_t n;
} span;

static bool eq_ci(span s, const char *lit)
{
    size_t n = strlen(lit);
    if (s.n != n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (lower(s.p[i]) != (uint8_t)lit[i]) {
            return false;
        }
    }
    return true;
}

static span trim(span s)
{
    while (s.n > 0 && is_ws(s.p[0])) {
        s.p++;
        s.n--;
    }
    while (s.n > 0 && is_ws(s.p[s.n - 1])) {
        s.n--;
    }
    return s;
}

/* Splits *rest at the first comma: returns the trimmed element before it. */
static span next_elem(span *rest)
{
    size_t i = 0;
    while (i < rest->n && rest->p[i] != ',') {
        i++;
    }
    span e = {rest->p, i};
    if (i < rest->n) {
        i++;
    }
    rest->p += i;
    rest->n -= i;
    return trim(e);
}

/* 1-19 decimal digits (fits in uint64_t). */
static bool parse_u64(span s, uint64_t *out)
{
    if (s.n == 0 || s.n > 19) {
        return false;
    }
    uint64_t v = 0;
    for (size_t i = 0; i < s.n; i++) {
        if (!is_digit(s.p[i])) {
            return false;
        }
        v = v * 10U + (uint64_t)(s.p[i] - '0');
    }
    *out = v;
    return true;
}

/* Content-Length: one value, or a list of equal values; repeats must agree too (§4.3). */
static bool on_content_length(span v, xc_http_response *r)
{
    span rest = v;
    do {
        uint64_t n = 0;
        if (!parse_u64(next_elem(&rest), &n)) {
            return false;
        }
        if (r->has_length && r->length != n) {
            return false;
        }
        r->has_length = true;
        r->length = n;
    } while (rest.n > 0);
    return true;
}

/* Transfer-Encoding: exactly one "chunked", nothing else (§4.3). */
static bool on_transfer_encoding(span v, xc_http_response *r)
{
    span rest = v;
    do {
        span e = next_elem(&rest);
        if (!eq_ci(e, "chunked") || r->chunked) {
            return false;
        }
        r->chunked = true;
    } while (rest.n > 0);
    return true;
}

static bool on_content_encoding(span v)
{
    span rest = v;
    do {
        if (!eq_ci(next_elem(&rest), "identity")) {
            return false;
        }
    } while (rest.n > 0);
    return true;
}

/* Splits s at the first `sep`; false if absent. */
static bool split(span s, uint8_t sep, span *a, span *b)
{
    for (size_t i = 0; i < s.n; i++) {
        if (s.p[i] == sep) {
            a->p = s.p;
            a->n = i;
            b->p = s.p + i + 1;
            b->n = s.n - i - 1;
            return true;
        }
    }
    return false;
}

/* Content-Range values (§4.3): a satisfied range with a known or unknown total, or an unsatisfied
 * one. */
static bool on_content_range(span v, xc_http_response *r)
{
    if (r->has_range || r->range_unsatisfied) {
        return false;
    }
    if (v.n < 6 || !eq_ci((span){v.p, 5}, "bytes") || v.p[5] != ' ') {
        return false;
    }
    span rest = {v.p + 6, v.n - 6};
    span range;
    span total;
    if (!split(rest, '/', &range, &total)) {
        return false;
    }
    if (range.n == 1 && range.p[0] == '*') {
        if (!parse_u64(total, &r->range_total)) {
            return false;
        }
        r->has_total = true;
        r->range_unsatisfied = true;
        return true;
    }
    span first;
    span last;
    if (!split(range, '-', &first, &last) || !parse_u64(first, &r->range_first) ||
        !parse_u64(last, &r->range_last) || r->range_first > r->range_last) {
        return false;
    }
    if (total.n == 1 && total.p[0] == '*') {
        r->has_total = false;
    } else {
        if (!parse_u64(total, &r->range_total) || r->range_last >= r->range_total) {
            return false;
        }
        r->has_total = true;
    }
    r->has_range = true;
    return true;
}

static void on_retry_after(span v, xc_http_response *r)
{
    if (v.n == 0 || v.n > 10) {
        return; /* HTTP-date or nonsense: ignored */
    }
    uint64_t s = 0;
    for (size_t i = 0; i < v.n; i++) {
        if (!is_digit(v.p[i])) {
            return;
        }
        s = s * 10U + (uint64_t)(v.p[i] - '0');
    }
    r->has_retry_after = true;
    r->retry_after_s = s > UINT32_MAX ? UINT32_MAX : (uint32_t)s;
}

static void on_connection(span v, xc_http_response *r)
{
    span rest = v;
    do {
        if (eq_ci(next_elem(&rest), "close")) {
            r->close = true;
        }
    } while (rest.n > 0);
}

static void store_validator(span v, char *out)
{
    if (v.n > XC_HTTP_MAX_VALIDATOR) {
        out[0] = '\0';
        return;
    }
    memcpy(out, v.p, v.n);
    out[v.n] = '\0';
}

/* "HTTP/1.x SSS[ reason]" (§4.1). */
static bool parse_status_line(span l, xc_http_response *r)
{
    static const char proto[] = "HTTP/1.";
    if (l.n < 12 || memcmp(l.p, proto, 7) != 0 || !is_digit(l.p[7]) || l.p[8] != ' ' ||
        !is_digit(l.p[9]) || !is_digit(l.p[10]) || !is_digit(l.p[11])) {
        return false;
    }
    if (l.n > 12 && l.p[12] != ' ') {
        return false;
    }
    for (size_t i = 12; i < l.n; i++) {
        if (!is_value_byte(l.p[i])) {
            return false;
        }
    }
    r->status = (l.p[9] - '0') * 100 + (l.p[10] - '0') * 10 + (l.p[11] - '0');
    return r->status >= 200 && r->status <= 599; /* 1xx: never expected (§4.1) */
}

static bool parse_header(span l, xc_http_response *r)
{
    size_t colon = 0;
    while (colon < l.n && l.p[colon] != ':') {
        if (!is_tchar(l.p[colon])) {
            return false; /* includes whitespace before the colon, and obs-fold */
        }
        colon++;
    }
    if (colon == 0 || colon == l.n) {
        return false;
    }
    span name = {l.p, colon};
    span value = {l.p + colon + 1, l.n - colon - 1};
    for (size_t i = 0; i < value.n; i++) {
        if (!is_value_byte(value.p[i])) {
            return false;
        }
    }
    value = trim(value);
    if (eq_ci(name, "content-length")) {
        return on_content_length(value, r);
    }
    if (eq_ci(name, "transfer-encoding")) {
        return on_transfer_encoding(value, r);
    }
    if (eq_ci(name, "content-encoding")) {
        return on_content_encoding(value);
    }
    if (eq_ci(name, "content-range")) {
        return on_content_range(value, r);
    }
    if (eq_ci(name, "location")) {
        if (r->location != NULL) {
            return false;
        }
        r->location = (const char *)value.p;
        r->location_len = value.n;
        return true;
    }
    if (eq_ci(name, "retry-after")) {
        on_retry_after(value, r);
    } else if (eq_ci(name, "connection")) {
        on_connection(value, r);
    } else if (eq_ci(name, "etag")) {
        store_validator(value, r->etag);
    } else if (eq_ci(name, "last-modified")) {
        store_validator(value, r->last_modified);
    }
    return true;
}

xc_err xc_http_parse_head(const uint8_t *buf, size_t len, xc_http_response *out)
{
    if (buf == NULL || out == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    if (len > XC_HTTP_MAX_HEAD) {
        return XC_ERR_HTTP_PROTOCOL;
    }
    size_t pos = 0;
    unsigned lines = 0;
    bool ended = false;
    while (pos < len && !ended) {
        size_t eol = pos;
        while (eol < len && buf[eol] != '\n') {
            eol++;
        }
        if (eol == len) {
            return XC_ERR_HTTP_PROTOCOL; /* no blank line */
        }
        span l = {buf + pos, eol - pos};
        if (l.n > 0 && l.p[l.n - 1] == '\r') {
            l.n--;
        }
        pos = eol + 1;
        if (lines == 0) {
            if (!parse_status_line(l, out)) {
                return XC_ERR_HTTP_PROTOCOL;
            }
        } else if (l.n == 0) {
            ended = true;
        } else if (lines > XC_HTTP_MAX_HEADERS || !parse_header(l, out)) {
            return XC_ERR_HTTP_PROTOCOL;
        }
        lines++;
    }
    if (!ended || pos != len || (out->chunked && out->has_length)) {
        return XC_ERR_HTTP_PROTOCOL;
    }
    return XC_OK;
}

/* --- chunked (§5) ------------------------------------------------------------------- */

enum {
    CH_SIZE,    /* hex digits of chunk-size */
    CH_SIZE_WS, /* whitespace after the digits */
    CH_EXT,     /* chunk extension, ignored */
    CH_SIZE_LF, /* CR seen at the end of the size line */
    CH_DATA,    /* chunk data */
    CH_DATA_CR, /* CRLF after the data */
    CH_DATA_LF,
    CH_TRAILER, /* start of a trailer line, or the final blank line */
    CH_TRAILER_LINE,
    CH_END_LF, /* CR of the final blank line seen */
    CH_DONE,
    CH_ERROR
};

#define CH_MAX_SIZE UINT64_C(0x7FFFFFFFFFFFFFFF)

void xc_http_chunked_init(xc_http_chunked *c)
{
    memset(c, 0, sizeof *c);
    c->state = CH_SIZE;
}

bool xc_http_chunked_done(const xc_http_chunked *c)
{
    return c->state == CH_DONE;
}

static int hex_value(uint8_t b)
{
    if (is_digit(b)) {
        return b - '0';
    }
    b = lower(b);
    if (b >= 'a' && b <= 'f') {
        return b - 'a' + 10;
    }
    return -1;
}

/* End of a chunk-size line: data follows, or the trailer section after the last chunk. */
static int size_line_done(xc_http_chunked *c)
{
    if (c->digits == 0) {
        return CH_ERROR;
    }
    c->line = 0;
    return c->size == 0 ? CH_TRAILER : CH_DATA;
}

/* After a chunk's data and its line end: the next chunk-size line. */
static int next_chunk(xc_http_chunked *c)
{
    c->size = 0;
    c->digits = 0;
    c->line = 0;
    return CH_SIZE;
}

/* Advances the state machine by one non-data byte. */
static int step(xc_http_chunked *c, uint8_t b)
{
    switch (c->state) {
    case CH_SIZE: {
        int h = hex_value(b);
        if (h >= 0) {
            if (c->digits == 16 || c->size > (CH_MAX_SIZE >> 4U)) {
                return CH_ERROR;
            }
            c->size = (c->size << 4U) | (uint64_t)h;
            c->digits++;
            return CH_SIZE;
        }
        if (c->digits == 0) {
            return CH_ERROR;
        }
        if (is_ws(b)) {
            return CH_SIZE_WS;
        }
        if (b == ';') {
            return CH_EXT;
        }
        if (b == '\r') {
            return CH_SIZE_LF;
        }
        return b == '\n' ? size_line_done(c) : CH_ERROR;
    }
    case CH_SIZE_WS:
        if (is_ws(b)) {
            return CH_SIZE_WS;
        }
        if (b == ';') {
            return CH_EXT;
        }
        if (b == '\r') {
            return CH_SIZE_LF;
        }
        return b == '\n' ? size_line_done(c) : CH_ERROR;
    case CH_EXT:
        if (b == '\r') {
            return CH_SIZE_LF;
        }
        if (b == '\n') {
            return size_line_done(c);
        }
        return is_value_byte(b) ? CH_EXT : CH_ERROR;
    case CH_SIZE_LF:
        return b == '\n' ? size_line_done(c) : CH_ERROR;
    case CH_DATA_CR:
        if (b == '\r') {
            return CH_DATA_LF;
        }
        return b == '\n' ? next_chunk(c) : CH_ERROR; /* a bare LF also ends the data */
    case CH_DATA_LF:
        return b == '\n' ? next_chunk(c) : CH_ERROR;
    case CH_TRAILER:
        if (b == '\r') {
            return CH_END_LF;
        }
        if (b == '\n') {
            return CH_DONE;
        }
        return is_value_byte(b) ? CH_TRAILER_LINE : CH_ERROR;
    case CH_TRAILER_LINE:
        if (b == '\n') {
            c->line = 0;
            return CH_TRAILER;
        }
        return (is_value_byte(b) || b == '\r') ? CH_TRAILER_LINE : CH_ERROR;
    case CH_END_LF:
        return b == '\n' ? CH_DONE : CH_ERROR;
    default:
        return CH_ERROR;
    }
}

xc_err xc_http_chunked_feed(xc_http_chunked *c, const uint8_t *in, size_t len, size_t *used,
                            const uint8_t **data, size_t *data_len)
{
    if (c == NULL || (in == NULL && len > 0) || used == NULL || data == NULL || data_len == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    *used = 0;
    *data = NULL;
    *data_len = 0;
    size_t i = 0;
    while (i < len && c->state != CH_DONE) {
        if (c->state == CH_ERROR) {
            return XC_ERR_HTTP_PROTOCOL;
        }
        if (c->state == CH_DATA) {
            size_t n = len - i;
            if ((uint64_t)n > c->size) {
                n = (size_t)c->size;
            }
            c->size -= n;
            if (c->size == 0) {
                c->state = CH_DATA_CR;
            }
            *data = in + i;
            *data_len = n;
            *used = i + n;
            return XC_OK;
        }
        bool in_trailer = c->state == CH_TRAILER || c->state == CH_TRAILER_LINE;
        c->line++;
        if (c->line > XC_HTTP_MAX_LINE) {
            c->state = CH_ERROR;
            return XC_ERR_HTTP_PROTOCOL;
        }
        if (in_trailer && ++c->trailer > XC_HTTP_MAX_TRAILERS) {
            c->state = CH_ERROR;
            return XC_ERR_HTTP_PROTOCOL;
        }
        c->state = step(c, in[i]);
        i++;
    }
    *used = i;
    return c->state == CH_ERROR ? XC_ERR_HTTP_PROTOCOL : XC_OK;
}
