#ifndef XCAT_SINK_H
#define XCAT_SINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xcat/error.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Where extracted files go: a host directory, the Xbox HDD (FATX, M2), or memory
 * in tests (docs/ARCHITECTURE.md §4). Paths are '/'-separated, relative to the
 * sink's root, and already validated by the reader (docs/formats/xdvdfs.md §4).
 * At most one file is open at a time.
 */
typedef struct xc_sink {
    /* Creates a directory. Its parent has already been created (or is the root). */
    xc_err (*make_dir)(void *ctx, const char *path);
    /* Creates (or truncates) a file that will receive exactly `size` bytes. */
    xc_err (*begin_file)(void *ctx, const char *path, uint32_t size);
    /* Appends to the open file. */
    xc_err (*write)(void *ctx, const void *buf, size_t len);
    /*
     * Closes the open file. `complete` is false after an error or cancellation;
     * the sink should then remove or mark the partial file.
     */
    xc_err (*end_file)(void *ctx, bool complete);
    void *ctx;
} xc_sink;

#ifdef __cplusplus
}
#endif

#endif /* XCAT_SINK_H */
