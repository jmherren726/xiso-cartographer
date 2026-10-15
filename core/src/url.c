/* URLs for the HTTP client: docs/formats/http.md §2. */
#include "xcat/url.h"

#include <string.h>

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static bool is_alpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool is_hex(char c)
{
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static bool prefix_ci(const char *s, size_t len, const char *p)
{
    size_t n = strlen(p);
    if (len < n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (lower(s[i]) != p[i]) {
            return false;
        }
    }
    return true;
}

/* §2.1: no control characters, and no fragment. */
static bool chars_ok(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20U || c == 0x7FU || c == '#') {
            return false;
        }
    }
    return true;
}

/* Four decimal numbers 0-255 without leading zeros (§2.1). */
static bool parse_ipv4(const char *s, size_t len, uint8_t out[4])
{
    size_t i = 0;
    for (int part = 0; part < 4; part++) {
        if (part > 0) {
            if (i >= len || s[i] != '.') {
                return false;
            }
            i++;
        }
        size_t start = i;
        unsigned v = 0;
        while (i < len && is_digit(s[i]) && i - start < 3) {
            v = v * 10U + (unsigned)(s[i] - '0');
            i++;
        }
        size_t digits = i - start;
        if (digits == 0 || v > 255U || (digits > 1 && s[start] == '0')) {
            return false;
        }
        out[part] = (uint8_t)v;
    }
    return i == len;
}

/* reg-name: letters, digits, '-' and '.', no empty labels; or an IPv4 literal (§2.1). */
static bool host_ok(const char *s, size_t len, bool *is_ipv4)
{
    uint8_t ip[4];
    *is_ipv4 = false;
    if (len == 0 || len > XC_URL_MAX_HOST) {
        return false;
    }
    size_t last_label = 0;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c == '.') {
            if (i == 0 || s[i - 1] == '.' || i + 1 == len) {
                return false;
            }
            last_label = i + 1;
        } else if (!is_alpha(c) && !is_digit(c) && c != '-') {
            return false;
        }
    }
    if (is_digit(s[last_label])) {
        /* A numeric last label must be a real address, never something a resolver reinterprets. */
        *is_ipv4 = parse_ipv4(s, len, ip);
        return *is_ipv4;
    }
    return true;
}

static bool needs_escape(unsigned char c)
{
    return c == ' ' || c == '"' || c == '<' || c == '>' || c == '\\' || c == '^' || c == '`' ||
           c == '{' || c == '|' || c == '}' || c >= 0x80U;
}

/*
 * Percent-encodes u->target[0, len) in place (§2.2), expanding from the back so
 * no second buffer is needed. `next1`/`next2` remember the original bytes after
 * position i, because the expansion may already have overwritten them.
 */
static xc_err encode_target(xc_url *u, size_t len)
{
    char *t = u->target;
    size_t extra = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)t[i];
        bool stray_pct = c == '%' && !(i + 2 < len && is_hex(t[i + 1]) && is_hex(t[i + 2]));
        if (needs_escape(c) || stray_pct) {
            extra += 2;
        }
    }
    if (len + extra > XC_URL_MAX_TARGET) {
        return XC_ERR_URL;
    }
    static const char hex[] = "0123456789ABCDEF";
    size_t w = len + extra;
    t[w] = '\0';
    char next1 = '\0';
    char next2 = '\0';
    bool have1 = false;
    bool have2 = false;
    for (size_t i = len; i-- > 0;) {
        char c = t[i];
        unsigned char uc = (unsigned char)c;
        bool stray_pct = c == '%' && !(have1 && have2 && is_hex(next1) && is_hex(next2));
        if (needs_escape(uc) || stray_pct) {
            t[--w] = hex[uc & 0x0FU];
            t[--w] = hex[uc >> 4U];
            t[--w] = '%';
        } else {
            t[--w] = c;
        }
        next2 = next1;
        have2 = have1;
        next1 = c;
        have1 = true;
    }
    return XC_OK;
}

/*
 * RFC 3986 §5.2.4 for an absolute path in t[0, len), in place: the output index
 * never passes the input index. Returns the new length.
 */
static size_t remove_dot_segments(char *t, size_t len)
{
    size_t o = 0;
    size_t i = 0;
    bool trailing_slash = false;
    while (i < len) {
        /* t[i] == '/': the segment is (i, end). */
        size_t end = i + 1;
        while (end < len && t[end] != '/') {
            end++;
        }
        size_t seg = end - (i + 1);
        const char *s = t + i + 1;
        if (seg == 1 && s[0] == '.') {
            trailing_slash = true;
        } else if (seg == 2 && s[0] == '.' && s[1] == '.') {
            while (o > 0 && t[o - 1] != '/') {
                o--;
            }
            if (o > 0) {
                o--; /* drop the '/' too */
            }
            trailing_slash = true;
        } else {
            t[o++] = '/';
            for (size_t k = 0; k < seg; k++) {
                t[o++] = s[k];
            }
            trailing_slash = false;
        }
        i = end;
    }
    if (trailing_slash || o == 0) {
        t[o++] = '/';
    }
    return o;
}

/*
 * u->target[0, plen) holds a raw absolute path. Removes dot segments, appends
 * '?' + query when `has_query`, and percent-encodes the result.
 */
static xc_err finish_target(xc_url *u, size_t plen, const char *query, size_t qlen, bool has_query)
{
    size_t len = remove_dot_segments(u->target, plen);
    if (has_query) {
        if (len + 1 + qlen > XC_URL_MAX_TARGET) {
            return XC_ERR_URL;
        }
        u->target[len++] = '?';
        memcpy(u->target + len, query, qlen);
        len += qlen;
    }
    return encode_target(u, len);
}

/* Appends s[0, n) to u->target at *pos. */
static bool put(xc_url *u, size_t *pos, const char *s, size_t n)
{
    if (n > XC_URL_MAX_TARGET - *pos) {
        return false;
    }
    memcpy(u->target + *pos, s, n);
    *pos += n;
    return true;
}

/* Parses everything after "<scheme>://". */
static xc_err parse_rest(xc_url_scheme scheme, const char *s, size_t len, xc_url *out)
{
    size_t auth_end = 0;
    while (auth_end < len && s[auth_end] != '/' && s[auth_end] != '?') {
        auth_end++;
    }
    size_t host_end = 0;
    while (host_end < auth_end && s[host_end] != ':') {
        if (s[host_end] == '@' || s[host_end] == '[') {
            return XC_ERR_URL; /* credentials in the URL, or IPv6 */
        }
        host_end++;
    }
    for (size_t i = host_end; i < auth_end; i++) {
        if (s[i] == '@' || s[i] == '[') {
            return XC_ERR_URL;
        }
    }
    uint16_t port = scheme == XC_URL_HTTPS ? 443U : 80U;
    if (host_end < auth_end) {
        size_t digits = auth_end - host_end - 1;
        if (digits == 0 || digits > 5) {
            return XC_ERR_URL;
        }
        unsigned long v = 0;
        for (size_t i = host_end + 1; i < auth_end; i++) {
            if (!is_digit(s[i])) {
                return XC_ERR_URL;
            }
            v = v * 10UL + (unsigned long)(s[i] - '0');
        }
        if (v == 0 || v > 65535UL) {
            return XC_ERR_URL;
        }
        port = (uint16_t)v;
    }
    bool ipv4 = false;
    if (!host_ok(s, host_end, &ipv4)) {
        return XC_ERR_URL;
    }
    out->scheme = scheme;
    out->port = port;
    out->is_ipv4 = ipv4;
    for (size_t i = 0; i < host_end; i++) {
        out->host[i] = lower(s[i]);
    }
    out->host[host_end] = '\0';

    const char *pq = s + auth_end;
    size_t pq_len = len - auth_end;
    size_t q = 0;
    while (q < pq_len && pq[q] != '?') {
        q++;
    }
    size_t pos = 0;
    if (q == 0) {
        out->target[pos++] = '/';
    } else if (!put(out, &pos, pq, q)) {
        return XC_ERR_URL;
    }
    bool has_query = q < pq_len;
    return finish_target(out, pos, has_query ? pq + q + 1 : NULL, has_query ? pq_len - q - 1 : 0,
                         has_query);
}

/* Matches "http://" or "https://" at the start; returns the prefix length, or 0. */
static size_t scheme_prefix(const char *s, size_t len, xc_url_scheme *scheme)
{
    if (prefix_ci(s, len, "http://")) {
        *scheme = XC_URL_HTTP;
        return 7;
    }
    if (prefix_ci(s, len, "https://")) {
        *scheme = XC_URL_HTTPS;
        return 8;
    }
    return 0;
}

xc_err xc_url_parse(const char *text, size_t len, xc_url *out)
{
    if (text == NULL || out == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    if (len == 0 || len > XC_URL_MAX || !chars_ok(text, len)) {
        return XC_ERR_URL;
    }
    xc_url_scheme scheme = XC_URL_HTTP;
    size_t n = scheme_prefix(text, len, &scheme);
    if (n == 0) {
        return XC_ERR_URL;
    }
    return parse_rest(scheme, text + n, len - n, out);
}

/* True if ref starts with a URI scheme ("x:" before any '/', '?'), RFC 3986 §3.1. */
static bool has_scheme(const char *ref, size_t len)
{
    if (len == 0 || !is_alpha(ref[0])) {
        return false;
    }
    for (size_t i = 1; i < len; i++) {
        char c = ref[i];
        if (c == ':') {
            return true;
        }
        if (!is_alpha(c) && !is_digit(c) && c != '+' && c != '-' && c != '.') {
            return false;
        }
    }
    return false;
}

xc_err xc_url_resolve(const xc_url *base, const char *ref, size_t ref_len, xc_url *out)
{
    if (base == NULL || ref == NULL || out == NULL || base == out) {
        return XC_ERR_INVALID_ARG;
    }
    /* §2.3: the fragment is dropped, then the rest must be a clean, non-empty reference. */
    for (size_t i = 0; i < ref_len; i++) {
        if (ref[i] == '#') {
            ref_len = i;
            break;
        }
    }
    if (ref_len == 0 || ref_len > XC_URL_MAX || !chars_ok(ref, ref_len)) {
        return XC_ERR_URL;
    }
    if (has_scheme(ref, ref_len)) {
        return xc_url_parse(ref, ref_len, out);
    }
    if (ref_len >= 2 && ref[0] == '/' && ref[1] == '/') {
        return parse_rest(base->scheme, ref + 2, ref_len - 2, out);
    }

    out->scheme = base->scheme;
    out->port = base->port;
    out->is_ipv4 = base->is_ipv4;
    memcpy(out->host, base->host, sizeof out->host);

    size_t rq = 0;
    while (rq < ref_len && ref[rq] != '?') {
        rq++;
    }
    bool ref_has_query = rq < ref_len;
    const char *bt = base->target;
    size_t base_path = strcspn(bt, "?");
    size_t pos = 0;
    if (rq == 0) {
        /* Empty path: keep the base path, and its query unless the reference has one. */
        if (!put(out, &pos, bt, base_path)) {
            return XC_ERR_URL;
        }
        if (ref_has_query) {
            return finish_target(out, pos, ref + 1, ref_len - 1, true);
        }
        bool base_query = bt[base_path] == '?';
        return finish_target(out, pos, base_query ? bt + base_path + 1 : NULL,
                             base_query ? strlen(bt + base_path + 1) : 0, base_query);
    }
    if (ref[0] != '/') {
        /* Relative path: merge with the base path up to its last '/'. */
        size_t dir = base_path;
        while (dir > 0 && bt[dir - 1] != '/') {
            dir--;
        }
        if (!put(out, &pos, bt, dir)) {
            return XC_ERR_URL;
        }
    }
    if (!put(out, &pos, ref, rq)) {
        return XC_ERR_URL;
    }
    return finish_target(out, pos, ref_has_query ? ref + rq + 1 : NULL,
                         ref_has_query ? ref_len - rq - 1 : 0, ref_has_query);
}

bool xc_url_default_port(const xc_url *url)
{
    return url->port == (url->scheme == XC_URL_HTTPS ? 443U : 80U);
}

bool xc_url_ipv4(const xc_url *url, uint8_t out[4])
{
    return url->is_ipv4 && parse_ipv4(url->host, strlen(url->host), out);
}
