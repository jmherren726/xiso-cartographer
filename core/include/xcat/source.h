#ifndef XCAT_SOURCE_H
#define XCAT_SOURCE_H

#include <stddef.h>
#include <stdint.h>

#include "xcat/error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Receives streamed bytes; `offset` is the source offset of data[0]. Non-OK stops the stream. */
typedef xc_err (*xc_source_data_fn)(void *ctx, uint64_t offset, const uint8_t *data, size_t len);

/*
 * A random-access, read-only byte source: a local file, an HTTP Range
 * endpoint, or memory (docs/ARCHITECTURE.md §4). The core never knows which.
 */
typedef struct xc_source {
    /*
     * Reads exactly `len` bytes at `offset` into `buf`. Must return XC_ERR_RANGE
     * if [offset, offset + len) isn't inside [0, size), and XC_ERR_IO (or
     * another error) on failure. Never returns a short read as success.
     */
    xc_err (*read_at)(void *ctx, uint64_t offset, void *buf, size_t len);
    void *ctx;
    uint64_t size; /* total size in bytes */
    /*
     * Optional (NULL if the source has none): streams [offset, offset + len) to
     * `fn` in order, each byte exactly once, in pieces of any size. An HTTP
     * source serves a whole span with one Range request this way (ADR 0004 §3),
     * instead of one request per read_at. The range must lie inside [0, size).
     */
    xc_err (*read_stream)(void *ctx, uint64_t offset, uint64_t len, xc_source_data_fn fn,
                          void *fn_ctx);
} xc_source;

/* Reads through a source after checking the range against its size. */
xc_err xc_source_read(const xc_source *src, uint64_t offset, void *buf, size_t len);

/* In-memory source, for tests and fuzzing. `mem` must outlive `src`. */
typedef struct xc_mem_source {
    const uint8_t *data;
    size_t len;
} xc_mem_source;

void xc_source_memory(xc_source *src, xc_mem_source *mem, const void *data, size_t len);

/* A window [base, base + size) of another source, seen from offset 0 (a stored zip member,
   docs/formats/zip.md §6). It streams when `inner` does. `win` must outlive `src`. */
typedef struct xc_window_source {
    const xc_source *inner;
    uint64_t base;
} xc_window_source;

void xc_source_window(xc_source *src, xc_window_source *win, const xc_source *inner, uint64_t base,
                      uint64_t size);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_SOURCE_H */
