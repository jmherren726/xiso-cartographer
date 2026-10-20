#ifndef XCAT_SEQ_H
#define XCAT_SEQ_H

/*
 * Sequential extraction (docs/formats/stream-install.md, ADR 0009 amendment 1): installs an
 * image that can only be read front to back, such as a Deflate zip member. Directory tables
 * are parsed as they pass; files whose directory is known are written as they pass; every
 * other byte that might belong to a file is spooled to disk, keeping the latest when space
 * runs out. When the stream ends, late files are copied out of the spool, and what the spool
 * dropped is read again in further passes. The platform provides the stream, the sink and
 * the spool storage.
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

#define XC_SEQ_DEFAULT_CHUNK ((uint64_t)64 * 1024 * 1024) /* image bytes per spool chunk */
#define XC_SEQ_DEFAULT_MAX_PASSES 16U

/*
 * Spool storage: numbered chunk files, each holding unidentified bytes of one interval of
 * `chunk_bytes` image bytes, appended in order (stream-install.md §4).
 */
typedef struct xc_seq_spool {
    /* Appends to chunk `chunk`, creating it if needed. */
    xc_err (*append)(void *ctx, uint32_t chunk, const uint8_t *data, size_t len);
    /* Reads `len` bytes at `pos` (bytes from the chunk's start) of chunk `chunk`. */
    xc_err (*read)(void *ctx, uint32_t chunk, uint64_t pos, uint8_t *buf, size_t len);
    /* Deletes chunk `chunk`. */
    xc_err (*drop)(void *ctx, uint32_t chunk);
    /* Free bytes on the drive that holds the spool and the game. */
    xc_err (*free_space)(void *ctx, uint64_t *bytes);
    void *ctx;
} xc_seq_spool;

/*
 * Delivers the image's bytes in order, each once, from an offset at or before `from` (a
 * checkpoint) to the end, through `fn`; stops and returns what `fn` returned if that isn't
 * XC_OK.
 */
typedef xc_err (*xc_seq_stream_fn)(void *ctx, uint64_t from, xc_source_data_fn fn, void *fn_ctx);

typedef struct xc_seq_progress {
    unsigned pass;        /* 1, 2, ... */
    uint64_t offset;      /* where the stream is */
    uint64_t image_size;  /* the stream's end */
    uint64_t spool_bytes; /* on disk now */
    size_t files_done;
    size_t file_count;   /* files known so far */
    bool all_known;      /* every directory table has been read */
    const char *current; /* listing path of the file being written, or NULL */
} xc_seq_progress;

typedef struct xc_seq_req {
    uint64_t image_size;
    xc_seq_stream_fn stream;
    void *stream_ctx;
    const xc_sink *sink;
    const xc_seq_spool *spool;
    uint64_t chunk_bytes;           /* 0 = XC_SEQ_DEFAULT_CHUNK */
    uint64_t reserve;               /* free bytes the install always leaves */
    uint32_t cluster_size;          /* allocation unit for space estimates; 0 = exact */
    const xc_xdvdfs_limits *limits; /* NULL = defaults */
    uint8_t *buf;                   /* I/O buffer, at least 64 KiB */
    size_t buf_size;
    unsigned max_passes; /* 0 = XC_SEQ_DEFAULT_MAX_PASSES */
    /* Read pass 1 to the stream's end even once nothing more is needed, e.g. so a zip
       member's CRC-32 covers every byte (zip.md §9). */
    bool full_first_pass;
    /* A file an earlier attempt already finished (journal.md §6.8): it isn't written, and
       file_done isn't called for it. May be NULL. */
    bool (*is_done)(void *ctx, const xc_xdvdfs_entry *file);
    /* If not NULL, gets the final listing on success; the caller frees it. */
    xc_xdvdfs_listing *listing_out;
    /*
     * Called after each directory table has been parsed, with everything known so far
     * (`complete` once every table is known), before any file under it is written; for
     * the FATX checks (ADR 0008). Non-OK stops the install. May be NULL.
     */
    xc_err (*on_listing)(void *ctx, const xc_xdvdfs_volume *vol, const xc_xdvdfs_listing *listing,
                         bool complete);
    /* After a file was written and ended (complete = true), with its CRC-32. May be NULL. */
    xc_err (*file_done)(void *ctx, const xc_xdvdfs_entry *file, uint32_t crc32);
    /* About every MiB and after each file. Return false to cancel (XC_ERR_CANCELLED). */
    bool (*progress)(void *ctx, const xc_seq_progress *p);
    void *ctx;
} xc_seq_req;

typedef struct xc_seq_result {
    xc_xdvdfs_volume vol; /* once found */
    unsigned passes;
    size_t file_count;       /* when every table is known */
    size_t files_written;    /* as they passed */
    size_t files_from_spool; /* copied out of the spool */
    size_t files_skipped;    /* is_done */
    uint64_t bytes_written;
    uint64_t streamed;      /* image bytes delivered, all passes */
    uint64_t reread;        /* of those, in passes 2 and later */
    uint64_t spooled;       /* bytes written to the spool */
    uint64_t spool_dropped; /* spooled bytes dropped for space before they were used */
    uint64_t spool_peak;
    uint64_t space_needed; /* XC_ERR_NO_SPACE */
    uint64_t space_free;
    char current_file[256]; /* on failure: the file being written */
} xc_seq_result;

/*
 * Runs the install (stream-install.md §3). XC_ERR_NOT_XDVDFS, XC_ERR_CORRUPT, XC_ERR_LIMIT
 * (entries, depth, or more than max_passes passes), XC_ERR_UNSUPPORTED (files that share
 * data), XC_ERR_NO_SPACE, XC_ERR_CANCELLED, or what the stream, sink, spool or a callback
 * returned. `res` may be NULL.
 */
xc_err xc_seq_extract(const xc_seq_req *req, xc_seq_result *res);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_SEQ_H */
