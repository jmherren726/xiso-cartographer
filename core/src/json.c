/* Strict, bounded JSON pull parser (json.h, docs/formats/archiveorg.md §6). */
#include "json.h"

#include <string.h>

#include "text.h"

enum {
    ST_VALUE,          /* a value: at the top, after ':' or after ',' in an array */
    ST_VALUE_OR_CLOSE, /* after '[' */
    ST_KEY,            /* after ',' in an object */
    ST_KEY_OR_CLOSE,   /* after '{' */
    ST_COLON,          /* after a key */
    ST_AFTER,          /* after a value inside a container: ',' or the closing bracket */
    ST_DONE,           /* the top value is complete */
    ST_FAILED,
};

void xc_json_init(xc_json *j, const char *data, size_t len)
{
    j->p = data;
    j->end = data + len;
    j->depth = 0;
    j->arrays = 0;
    j->state = ST_VALUE;
}

static void skip_ws(xc_json *j)
{
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) {
        j->p++;
    }
}

static bool in_array(const xc_json *j)
{
    return ((j->arrays >> (j->depth - 1U)) & 1U) != 0;
}

static xc_err fail(xc_json *j, xc_err err)
{
    j->state = ST_FAILED;
    return err;
}

static void after_value(xc_json *j)
{
    j->state = j->depth == 0 ? ST_DONE : ST_AFTER;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* j->p is at the opening quote. */
static xc_err scan_string(xc_json *j, xc_json_tok *t)
{
    const char *start = ++j->p;
    bool escaped = false;
    while (j->p < j->end && *j->p != '"') {
        unsigned char c = (unsigned char)*j->p;
        if (c < 0x20U) {
            return XC_ERR_CORRUPT;
        }
        if (c != '\\') {
            j->p++;
            continue;
        }
        escaped = true;
        if (j->end - j->p < 2) {
            return XC_ERR_CORRUPT;
        }
        char e = j->p[1];
        if (e == 'u') {
            if (j->end - j->p < 6) {
                return XC_ERR_CORRUPT;
            }
            for (int k = 2; k < 6; k++) {
                if (hex_digit(j->p[k]) < 0) {
                    return XC_ERR_CORRUPT;
                }
            }
            j->p += 6;
        } else if (strchr("\"\\/bfnrt", e) != NULL && e != '\0') {
            j->p += 2;
        } else {
            return XC_ERR_CORRUPT;
        }
    }
    if (j->p == j->end) {
        return XC_ERR_CORRUPT;
    }
    size_t len = (size_t)(j->p - start);
    size_t bad_at = 0;
    if (!xc_text_utf8((const unsigned char *)start, len, &bad_at)) {
        return XC_ERR_CORRUPT;
    }
    j->p++;
    t->raw = start;
    t->raw_len = len;
    t->escaped = escaped;
    return XC_OK;
}

static bool is_digit(const xc_json *j)
{
    return j->p < j->end && *j->p >= '0' && *j->p <= '9';
}

static xc_err scan_number(xc_json *j, xc_json_tok *t)
{
    const char *start = j->p;
    if (*j->p == '-') {
        j->p++;
    }
    if (!is_digit(j)) {
        return XC_ERR_CORRUPT;
    }
    if (*j->p == '0') {
        j->p++;
    } else {
        while (is_digit(j)) {
            j->p++;
        }
    }
    if (j->p < j->end && *j->p == '.') {
        j->p++;
        if (!is_digit(j)) {
            return XC_ERR_CORRUPT;
        }
        while (is_digit(j)) {
            j->p++;
        }
    }
    if (j->p < j->end && (*j->p == 'e' || *j->p == 'E')) {
        j->p++;
        if (j->p < j->end && (*j->p == '+' || *j->p == '-')) {
            j->p++;
        }
        if (!is_digit(j)) {
            return XC_ERR_CORRUPT;
        }
        while (is_digit(j)) {
            j->p++;
        }
    }
    t->raw = start;
    t->raw_len = (size_t)(j->p - start);
    return XC_OK;
}

static bool literal(xc_json *j, const char *word)
{
    size_t n = strlen(word);
    if ((size_t)(j->end - j->p) < n || memcmp(j->p, word, n) != 0) {
        return false;
    }
    j->p += n;
    return true;
}

static xc_err open_container(xc_json *j, xc_json_tok *t, bool array)
{
    if (j->depth >= XC_JSON_MAX_DEPTH) {
        return XC_ERR_LIMIT;
    }
    j->p++;
    j->depth++;
    uint32_t bit = 1U << (j->depth - 1U);
    j->arrays = array ? j->arrays | bit : j->arrays & ~bit;
    j->state = array ? ST_VALUE_OR_CLOSE : ST_KEY_OR_CLOSE;
    t->type = array ? XC_JSON_ARRAY : XC_JSON_OBJECT;
    return XC_OK;
}

static void close_container(xc_json *j, xc_json_tok *t)
{
    t->type = in_array(j) ? XC_JSON_ARRAY_END : XC_JSON_OBJECT_END;
    j->p++;
    j->depth--;
    after_value(j);
}

static xc_err value(xc_json *j, xc_json_tok *t)
{
    char c = *j->p;
    xc_err err = XC_OK;
    if (c == '{' || c == '[') {
        return open_container(j, t, c == '[');
    }
    if (c == '"') {
        t->type = XC_JSON_STRING;
        err = scan_string(j, t);
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        t->type = XC_JSON_NUMBER;
        err = scan_number(j, t);
    } else if (literal(j, "true")) {
        t->type = XC_JSON_TRUE;
    } else if (literal(j, "false")) {
        t->type = XC_JSON_FALSE;
    } else if (literal(j, "null")) {
        t->type = XC_JSON_NULL;
    } else {
        err = XC_ERR_CORRUPT;
    }
    if (err == XC_OK) {
        after_value(j);
    }
    return err;
}

static xc_err step(xc_json *j, xc_json_tok *t)
{
    for (;;) {
        skip_ws(j);
        if (j->state == ST_DONE) {
            if (j->p != j->end) {
                return XC_ERR_CORRUPT;
            }
            t->type = XC_JSON_END;
            return XC_OK;
        }
        if (j->p == j->end) {
            return XC_ERR_CORRUPT;
        }
        char c = *j->p;
        switch (j->state) {
        case ST_COLON:
            if (c != ':') {
                return XC_ERR_CORRUPT;
            }
            j->p++;
            j->state = ST_VALUE;
            continue;
        case ST_AFTER:
            if (c == ',') {
                j->p++;
                j->state = in_array(j) ? ST_VALUE : ST_KEY;
                continue;
            }
            if (c != (in_array(j) ? ']' : '}')) {
                return XC_ERR_CORRUPT;
            }
            close_container(j, t);
            return XC_OK;
        case ST_KEY_OR_CLOSE:
        case ST_KEY:
            if (c == '}' && j->state == ST_KEY_OR_CLOSE) {
                close_container(j, t);
                return XC_OK;
            }
            if (c != '"') {
                return XC_ERR_CORRUPT;
            }
            t->type = XC_JSON_KEY;
            j->state = ST_COLON;
            return scan_string(j, t);
        case ST_VALUE_OR_CLOSE:
            if (c == ']') {
                close_container(j, t);
                return XC_OK;
            }
            return value(j, t);
        case ST_VALUE:
            return value(j, t);
        default:
            return XC_ERR_CORRUPT;
        }
    }
}

xc_err xc_json_next(xc_json *j, xc_json_tok *t)
{
    memset(t, 0, sizeof *t);
    if (j->state == ST_FAILED) {
        return XC_ERR_CORRUPT;
    }
    xc_err err = step(j, t);
    return err == XC_OK ? XC_OK : fail(j, err);
}

xc_err xc_json_skip(xc_json *j, const xc_json_tok *first)
{
    if (first->type != XC_JSON_OBJECT && first->type != XC_JSON_ARRAY) {
        return XC_OK;
    }
    unsigned open = 1;
    while (open > 0) {
        xc_json_tok t;
        xc_err err = xc_json_next(j, &t);
        if (err != XC_OK) {
            return err;
        }
        if (t.type == XC_JSON_OBJECT || t.type == XC_JSON_ARRAY) {
            open++;
        } else if (t.type == XC_JSON_OBJECT_END || t.type == XC_JSON_ARRAY_END) {
            open--;
        }
    }
    return XC_OK;
}

static unsigned hex4(const char *p)
{
    unsigned v = 0;
    for (int k = 0; k < 4; k++) {
        v = (v << 4U) | (unsigned)hex_digit(p[k]);
    }
    return v;
}

/* Appends code point cp as UTF-8. */
static bool put_utf8(char *buf, size_t cap, size_t *n, uint32_t cp)
{
    unsigned char b[4];
    size_t k = 0;
    if (cp < 0x80U) {
        b[k++] = (unsigned char)cp;
    } else if (cp < 0x800U) {
        b[k++] = (unsigned char)(0xC0U | (cp >> 6U));
        b[k++] = (unsigned char)(0x80U | (cp & 0x3FU));
    } else if (cp < 0x10000U) {
        b[k++] = (unsigned char)(0xE0U | (cp >> 12U));
        b[k++] = (unsigned char)(0x80U | ((cp >> 6U) & 0x3FU));
        b[k++] = (unsigned char)(0x80U | (cp & 0x3FU));
    } else {
        b[k++] = (unsigned char)(0xF0U | (cp >> 18U));
        b[k++] = (unsigned char)(0x80U | ((cp >> 12U) & 0x3FU));
        b[k++] = (unsigned char)(0x80U | ((cp >> 6U) & 0x3FU));
        b[k++] = (unsigned char)(0x80U | (cp & 0x3FU));
    }
    if (cap - *n <= k) {
        return false;
    }
    memcpy(buf + *n, b, k);
    *n += k;
    return true;
}

xc_err xc_json_string(const xc_json_tok *t, char *buf, size_t cap, size_t *len)
{
    if (cap == 0 || (t->type != XC_JSON_STRING && t->type != XC_JSON_KEY)) {
        return XC_ERR_INVALID_ARG;
    }
    size_t n = 0;
    const char *p = t->raw;
    const char *end = t->raw + t->raw_len;
    while (p < end) {
        if (*p != '\\') {
            if (cap - n <= 1) {
                return XC_ERR_LIMIT;
            }
            buf[n++] = *p++;
            continue;
        }
        char e = p[1];
        if (e != 'u') {
            static const char from[] = "\"\\/bfnrt";
            static const char to[] = "\"\\/\b\f\n\r\t";
            if (cap - n <= 1) {
                return XC_ERR_LIMIT;
            }
            buf[n++] = to[strchr(from, e) - from];
            p += 2;
            continue;
        }
        uint32_t cp = hex4(p + 2);
        p += 6;
        if (cp >= 0xDC00U && cp <= 0xDFFFU) {
            return XC_ERR_CORRUPT; /* a low surrogate on its own */
        }
        if (cp >= 0xD800U && cp <= 0xDBFFU) {
            if (end - p < 6 || p[0] != '\\' || p[1] != 'u') {
                return XC_ERR_CORRUPT;
            }
            uint32_t lo = hex4(p + 2);
            if (lo < 0xDC00U || lo > 0xDFFFU) {
                return XC_ERR_CORRUPT;
            }
            cp = 0x10000U + ((cp - 0xD800U) << 10U) + (lo - 0xDC00U);
            p += 6;
        }
        if (cp == 0) {
            return XC_ERR_CORRUPT;
        }
        if (!put_utf8(buf, cap, &n, cp)) {
            return XC_ERR_LIMIT;
        }
    }
    buf[n] = '\0';
    if (len != NULL) {
        *len = n;
    }
    return XC_OK;
}

bool xc_json_equals(const xc_json_tok *t, const char *s)
{
    if (t->type != XC_JSON_STRING && t->type != XC_JSON_KEY) {
        return false;
    }
    size_t n = strlen(s);
    if (!t->escaped) {
        return t->raw_len == n && memcmp(t->raw, s, n) == 0;
    }
    char buf[64];
    size_t len = 0;
    return n < sizeof buf && xc_json_string(t, buf, sizeof buf, &len) == XC_OK && len == n &&
           memcmp(buf, s, n) == 0;
}
