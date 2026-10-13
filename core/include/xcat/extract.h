#ifndef XCAT_EXTRACT_H
#define XCAT_EXTRACT_H

/*
 * Extracts a listing from a source into a sink: directories first (parents
 * before children), then files in offset order. Design: ADR 0004.
 * Journal/resume and hash verification arrive with the network install (M4/M5).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xcat/error.h"
#include "xcat/sink.h"
#include "xcat/source.h"
#include "xcat/xdvdfs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Default for xc_extract_opts.coalesce_gap (ADR 0004 §3); retuned from the M3 benchmarks. */
#define XC_EXTRACT_DEFAULT_GAP ((uint64_t)1024 * 1024)

typedef struct xc_extract_progress {
    uint64_t bytes_done;  /* file bytes written so far */
    uint64_t bytes_total; /* listing->total_file_bytes */
    size_t files_done;
    size_t file_count;
    const char *current; /* path of the file being written */
} xc_extract_progress;

/* Called after each chunk is written. Return false to cancel (XC_ERR_CANCELLED). */
typedef bool (*xc_extract_progress_fn)(void *ctx, const xc_extract_progress *p);

typedef struct xc_extract_opts {
    uint8_t *buf;          /* I/O buffer, owned by the caller (allocated up front, §7.2) */
    size_t buf_size;       /* must be > 0; larger means fewer, bigger reads and writes */
    uint64_t coalesce_gap; /* read through gaps up to this size instead of jumping */
    xc_extract_progress_fn progress; /* may be NULL */
    void *progress_ctx;
} xc_extract_opts;

typedef struct xc_extract_stats {
    size_t dirs_created;
    size_t files_written;
    uint64_t bytes_written; /* file data */
    uint64_t bytes_read;    /* file data plus coalesced gaps */
    size_t spans;           /* contiguous read runs; each would be one HTTP Range request */
} xc_extract_stats;

/*
 * Reads are issued in offset order and are contiguous within a span, so a
 * streaming source (HTTP) can serve a span from a single request. On error the
 * file being written is ended with complete = false and extraction stops.
 * `stats` may be NULL; when given, it is filled in on success and on failure.
 */
xc_err xc_extract(const xc_source *src, const xc_xdvdfs_listing *listing, const xc_sink *sink,
                  const xc_extract_opts *opts, xc_extract_stats *stats);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_EXTRACT_H */
