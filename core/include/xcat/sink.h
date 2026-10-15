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
    /*
     * Optional (NULL if the sink has none; then no checkpoints are made, ADR 0011):
     * makes everything written to the open file so far durable, so it survives a
     * power cut.
     */
    xc_err (*sync)(void *ctx);
    /*
     * Optional: opens an existing file that will end up `size` bytes long, cut back
     * to its first `keep` bytes, for appending the rest. Fails if the file is
     * shorter than `keep`.
     */
    xc_err (*resume_file)(void *ctx, const char *path, uint32_t size, uint32_t keep);
    /*
     * Optional: closes the open, unfinished file but keeps what it has (instead of
     * end_file(complete = false), which removes it). Used after an error once the
     * file has a checkpoint, so the next attempt can continue it (ADR 0011).
     */
    xc_err (*suspend_file)(void *ctx);
} xc_sink;

#ifdef __cplusplus
}
#endif

#endif /* XCAT_SINK_H */
