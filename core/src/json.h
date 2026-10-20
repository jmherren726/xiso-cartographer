#ifndef XCAT_JSON_H
#define XCAT_JSON_H

/*
 * A strict, bounded JSON pull parser over a buffer in memory (docs/formats/archiveorg.md §6).
 * Tokens point into the buffer; strings are checked for syntax and UTF-8 as they are read
 * and only decoded on request, so unused parts cost nothing but a scan. Internal.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xcat/error.h"

#define XC_JSON_MAX_DEPTH 32

typedef enum xc_json_type {
    XC_JSON_END, /* the value is complete and only whitespace followed */
    XC_JSON_OBJECT,
    XC_JSON_OBJECT_END,
    XC_JSON_ARRAY,
    XC_JSON_ARRAY_END,
    XC_JSON_KEY, /* a member name; its value comes next */
    XC_JSON_STRING,
    XC_JSON_NUMBER,
    XC_JSON_TRUE,
    XC_JSON_FALSE,
    XC_JSON_NULL,
} xc_json_type;

typedef struct xc_json_tok {
    xc_json_type type;
    const char *raw; /* strings and keys: between the quotes, undecoded; numbers: the digits */
    size_t raw_len;
    bool escaped; /* a string or key with at least one backslash */
} xc_json_tok;

typedef struct xc_json {
    const char *p;
    const char *end;
    unsigned depth;
    uint32_t arrays; /* bit d - 1: the container at depth d is an array */
    int state;
} xc_json;

void xc_json_init(xc_json *j, const char *data, size_t len);

/* The next token. XC_ERR_CORRUPT for anything that isn't JSON (§6), XC_ERR_LIMIT when
 * nesting goes deeper than XC_JSON_MAX_DEPTH. After an error, keep returning it. */
xc_err xc_json_next(xc_json *j, xc_json_tok *t);

/* Skips the rest of the value that `first` (just returned by xc_json_next) starts. */
xc_err xc_json_skip(xc_json *j, const xc_json_tok *first);

/* Decodes a string or key into buf (NUL-terminated); *len (may be NULL) gets its length.
 * XC_ERR_LIMIT if it needs more than cap - 1 bytes, XC_ERR_CORRUPT for a bad \u surrogate
 * or \u0000. */
xc_err xc_json_string(const xc_json_tok *t, char *buf, size_t cap, size_t *len);

/* True if a string or key decodes to exactly `s`. */
bool xc_json_equals(const xc_json_tok *t, const char *s);

#endif /* XCAT_JSON_H */
