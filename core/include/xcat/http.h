#ifndef XCAT_HTTP_H
#define XCAT_HTTP_H

/*
 * HTTP/1.1 response parsing: the head (status line and headers) and the
 * chunked body decoding, as specified in docs/formats/http.md §4 and §5.
 * Pure functions over caller-provided bytes; the network side is the client.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xcat/error.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XC_HTTP_MAX_HEAD ((size_t)16 * 1024)     /* status line + headers + blank line (§1) */
#define XC_HTTP_MAX_HEADERS 100                  /* header lines per response (§1) */
#define XC_HTTP_MAX_LINE ((size_t)1024)          /* chunk-size line, trailer line (§1) */
#define XC_HTTP_MAX_TRAILERS ((size_t)16 * 1024) /* trailer section (§1) */
#define XC_HTTP_MAX_VALIDATOR 127                /* longest ETag / Last-Modified kept (§4.3) */

typedef struct xc_http_response {
    int status; /* 200-599 */

    bool has_length; /* Content-Length */
    uint64_t length;
    bool chunked; /* Transfer-Encoding: chunked */

    bool has_range; /* Content-Range: bytes first-last/(total|*) */
    uint64_t range_first;
    uint64_t range_last;
    bool has_total; /* total known: from "/total", or from "bytes *\/total" */
    uint64_t range_total;
    bool range_unsatisfied; /* Content-Range: bytes *\/total */

    bool close; /* Connection: close */

    bool has_retry_after; /* Retry-After in seconds; the date form is ignored */
    uint32_t retry_after_s;

    const char *location; /* points into the parsed buffer; NULL if absent */
    size_t location_len;

    char etag[XC_HTTP_MAX_VALIDATOR + 1];          /* "" if absent or too long */
    char last_modified[XC_HTTP_MAX_VALIDATOR + 1]; /* "" if absent or too long */
} xc_http_response;

/*
 * Length of the head at the start of buf[0, len), including the blank line that
 * ends it (§4.1), or 0 if the blank line hasn't arrived yet.
 */
size_t xc_http_head_end(const uint8_t *buf, size_t len);

/*
 * Parses a complete head, buf[0, len) as measured by xc_http_head_end().
 * Returns XC_ERR_HTTP_PROTOCOL if it breaks §4.
 */
xc_err xc_http_parse_head(const uint8_t *buf, size_t len, xc_http_response *out);

/* Incremental decoder for Transfer-Encoding: chunked (§5). */
typedef struct xc_http_chunked {
    int state;
    uint64_t size;   /* chunk size being parsed, or data left in the current chunk */
    unsigned digits; /* hex digits read for the current size */
    size_t line;     /* bytes in the current size or trailer line */
    size_t trailer;  /* bytes in the trailer section */
} xc_http_chunked;

void xc_http_chunked_init(xc_http_chunked *c);

/*
 * Feeds in[0, len). Stops at the first run of chunk data, which is returned in
 * *data / *data_len (pointing into `in`), or when the input is used up, or when
 * the body is complete. *used is the number of input bytes consumed, including
 * any data returned. Call again with the remaining input until *used == len or
 * the body is done; bytes after the end of the body are never consumed.
 * Returns XC_ERR_HTTP_PROTOCOL on malformed input; the decoder is then unusable.
 */
xc_err xc_http_chunked_feed(xc_http_chunked *c, const uint8_t *in, size_t len, size_t *used,
                            const uint8_t **data, size_t *data_len);

/* True once the last chunk and the trailer section have been read. */
bool xc_http_chunked_done(const xc_http_chunked *c);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_HTTP_H */
