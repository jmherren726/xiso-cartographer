#ifndef XCAT_SOURCE_H
#define XCAT_SOURCE_H

#include <stddef.h>
#include <stdint.h>

#include "xcat/error.h"

#ifdef __cplusplus
extern "C" {
#endif

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
} xc_source;

/* Reads through a source after checking the range against its size. */
xc_err xc_source_read(const xc_source *src, uint64_t offset, void *buf, size_t len);

/* In-memory source, for tests and fuzzing. `mem` must outlive `src`. */
typedef struct xc_mem_source {
    const uint8_t *data;
    size_t len;
} xc_mem_source;

void xc_source_memory(xc_source *src, xc_mem_source *mem, const void *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_SOURCE_H */
