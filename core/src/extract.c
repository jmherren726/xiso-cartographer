/*
 * Extractor: listing + source -> sink. Design: ADR 0004 (stream-extract,
 * offset order, coalesced spans). All data passes through the caller's buffer.
 */
#include "xcat/extract.h"

#include <string.h>

#define NO_POS UINT64_MAX

typedef struct run {
    const xc_source *src;
    const xc_sink *sink;
    const xc_extract_opts *opts;
    xc_extract_stats st;
    xc_extract_progress p;
} run;

static xc_err report(run *r)
{
    if (r->opts->progress != NULL && !r->opts->progress(r->opts->progress_ctx, &r->p)) {
        return XC_ERR_CANCELLED;
    }
    return XC_OK;
}

/* Reads and discards [pos, pos + len): a coalesced gap (ADR 0004 §3). */
static xc_err skip(run *r, uint64_t pos, uint64_t len)
{
    while (len > 0) {
        size_t n = len < r->opts->buf_size ? (size_t)len : r->opts->buf_size;
        xc_err err = xc_source_read(r->src, pos, r->opts->buf, n);
        if (err != XC_OK) {
            return err;
        }
        r->st.bytes_read += n;
        pos += n;
        len -= n;
    }
    return XC_OK;
}

static xc_err copy(run *r, uint64_t pos, uint32_t size)
{
    uint64_t left = size;
    while (left > 0) {
        size_t n = left < r->opts->buf_size ? (size_t)left : r->opts->buf_size;
        xc_err err = xc_source_read(r->src, pos, r->opts->buf, n);
        if (err != XC_OK) {
            return err;
        }
        r->st.bytes_read += n;
        err = r->sink->write(r->sink->ctx, r->opts->buf, n);
        if (err != XC_OK) {
            return err;
        }
        r->st.bytes_written += n;
        r->p.bytes_done += n;
        pos += n;
        left -= n;
        err = report(r);
        if (err != XC_OK) {
            return err;
        }
    }
    return XC_OK;
}

static bool opts_ok(const xc_sink *sink, const xc_extract_opts *opts)
{
    return sink->make_dir != NULL && sink->begin_file != NULL && sink->write != NULL &&
           sink->end_file != NULL && opts->buf != NULL && opts->buf_size > 0;
}

xc_err xc_extract(const xc_source *src, const xc_xdvdfs_listing *listing, const xc_sink *sink,
                  const xc_extract_opts *opts, xc_extract_stats *stats)
{
    if (stats != NULL) {
        memset(stats, 0, sizeof *stats);
    }
    if (src == NULL || listing == NULL || sink == NULL || opts == NULL || !opts_ok(sink, opts)) {
        return XC_ERR_INVALID_ARG;
    }
    run r;
    memset(&r, 0, sizeof r);
    r.src = src;
    r.sink = sink;
    r.opts = opts;
    r.p.bytes_total = listing->total_file_bytes;
    r.p.file_count = listing->file_count;

    xc_err err = XC_OK;
    /* The listing orders directories parents-first (docs/formats/xdvdfs.md §7). */
    for (size_t i = 0; err == XC_OK && i < listing->dir_count; i++) {
        err = sink->make_dir(sink->ctx, listing->dirs[i].path);
        if (err == XC_OK) {
            r.st.dirs_created++;
        }
    }

    uint64_t pos = NO_POS; /* where the current span's next read starts */
    for (size_t i = 0; err == XC_OK && i < listing->file_count; i++) {
        const xc_xdvdfs_entry *f = &listing->files[i];
        r.p.current = f->path;
        err = sink->begin_file(sink->ctx, f->path, f->size);
        if (err != XC_OK) {
            break;
        }
        if (f->size > 0) {
            /* Files are offset-sorted. Continue the span through a small gap; jump otherwise
               (large gap, or a file overlapping data already read). */
            if (pos != NO_POS && f->offset >= pos && f->offset - pos <= opts->coalesce_gap) {
                err = skip(&r, pos, f->offset - pos);
            } else {
                r.st.spans++;
            }
            if (err == XC_OK) {
                err = copy(&r, f->offset, f->size);
                pos = f->offset + f->size;
            }
        }
        if (err != XC_OK) {
            (void)sink->end_file(sink->ctx, false);
            break;
        }
        err = sink->end_file(sink->ctx, true);
        if (err == XC_OK) {
            r.st.files_written++;
            r.p.files_done++;
            err = report(&r);
        }
    }

    if (stats != NULL) {
        *stats = r.st;
    }
    return err;
}
