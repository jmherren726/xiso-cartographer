#ifndef XCAT_EXTRACT_H
#define XCAT_EXTRACT_H

/*
 * Extracts a listing from a source into a sink: directories first (parents
 * before children), then files in offset order, in coalesced spans. Design:
 * ADR 0004. Resume is supported by skipping files that are already installed
 * (the journal, xcat/journal.h, says which); xcat/install.h drives a whole
 * install around this.
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
    const bool *skip; /* [file index]: true = already installed, leave it alone; may be NULL */
    /*
     * Called after a file was written and its sink end_file(complete = true)
     * returned, with the CRC-32 of its content (xcat/crc32.h). Non-OK stops the
     * extraction. May be NULL.
     */
    xc_err (*file_done)(void *ctx, size_t index, uint32_t crc32);
    void *file_done_ctx;
} xc_extract_opts;

typedef struct xc_extract_stats {
    size_t dirs_created;
    size_t files_written;
    size_t files_skipped;   /* opts->skip */
    uint64_t bytes_written; /* file data */
    uint64_t bytes_read;    /* file data plus coalesced gaps */
    size_t spans; /* contiguous read runs; with read_stream, one HTTP Range request each */
} xc_extract_stats;

/*
 * Each span is read front to back: with one src->read_stream call when the
 * source has one (HTTP: one Range request per span), else in buf_size read_at
 * calls. Streamed bytes are gathered in `buf` so the sink gets large writes. On
 * error the file being written is ended with complete = false and extraction
 * stops.
 * `stats` may be NULL; when given, it is filled in on success and on failure.
 */
xc_err xc_extract(const xc_source *src, const xc_xdvdfs_listing *listing, const xc_sink *sink,
                  const xc_extract_opts *opts, xc_extract_stats *stats);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_EXTRACT_H */
