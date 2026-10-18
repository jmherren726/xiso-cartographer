/*
 * Extractor: listing + source -> sink. Design: ADR 0004 (stream-extract,
 * offset order, coalesced spans, resume by skipping finished files).
 *
 * Files are grouped into spans: runs of files in offset order whose gaps are at
 * most coalesce_gap. Each span is read front to back, either with one
 * read_stream call (HTTP: one Range request per span) or in buffer-sized
 * read_at calls, and its bytes are routed to the files they belong to; gap
 * bytes are read and discarded.
 */
#include "xcat/extract.h"

#include <string.h>

#include "xcat/crc32.h"

typedef struct run {
    const xc_source *src;
    const xc_xdvdfs_listing *l;
    const xc_sink *sink;
    const xc_extract_opts *opts;
    xc_extract_stats st;
    xc_extract_progress p;
    /* the span being read */
    size_t cur; /* listing index of the file the next bytes go to */
    size_t end; /* one past the span's last file */
    bool open;  /* files[cur] has been begun */
    uint32_t written;
    uint32_t crc;
    bool direct;        /* bytes arrive in opts->buf (read_at): write them straight through */
    bool keep_on_error; /* the open file has a checkpoint (or was resumed): never delete it */
    size_t wlen;        /* bytes waiting in opts->buf (read_stream) */
} run;

static bool skipped(const run *r, size_t i)
{
    return r->opts->skip != NULL && r->opts->skip[i];
}

/* ADR 0011: bytes of file i already on disk from an earlier attempt (0: none). */
static uint32_t resumed(const run *r, size_t i)
{
    if (r->opts->resume_bytes == NULL || r->sink->resume_file == NULL) {
        return 0;
    }
    uint32_t keep = r->opts->resume_bytes[i];
    return keep < r->l->files[i].size ? keep : 0;
}

static xc_err report(run *r)
{
    if (r->opts->progress != NULL && !r->opts->progress(r->opts->progress_ctx, &r->p)) {
        return XC_ERR_CANCELLED;
    }
    return XC_OK;
}

static xc_err sink_write(run *r, const uint8_t *data, size_t len)
{
    xc_err err = r->sink->write(r->sink->ctx, data, len);
    if (err == XC_OK) {
        r->st.bytes_written += len;
    }
    return err;
}

/* Streamed bytes are gathered into opts->buf, so the sink sees large writes (FATX is slow
   with small ones). */
static xc_err flush(run *r)
{
    xc_err err = r->wlen > 0 ? sink_write(r, r->opts->buf, r->wlen) : XC_OK;
    r->wlen = 0;
    return err;
}

static xc_err put(run *r, const uint8_t *data, size_t len)
{
    if (r->direct) {
        return sink_write(r, data, len);
    }
    while (len > 0) {
        if (r->wlen == 0 && len >= r->opts->buf_size) {
            return sink_write(r, data, len);
        }
        size_t n = r->opts->buf_size - r->wlen < len ? r->opts->buf_size - r->wlen : len;
        memcpy(r->opts->buf + r->wlen, data, n);
        r->wlen += n;
        data += n;
        len -= n;
        if (r->wlen == r->opts->buf_size) {
            xc_err err = flush(r);
            if (err != XC_OK) {
                return err;
            }
        }
    }
    return XC_OK;
}

static xc_err begin(run *r)
{
    const xc_xdvdfs_entry *f = &r->l->files[r->cur];
    r->p.current = f->path;
    uint32_t keep = resumed(r, r->cur);
    xc_err err = keep > 0 ? r->sink->resume_file(r->sink->ctx, f->path, f->size, keep)
                          : r->sink->begin_file(r->sink->ctx, f->path, f->size);
    if (err == XC_OK) {
        r->open = true;
        r->written = keep;
        r->crc = keep > 0 ? r->opts->resume_crc[r->cur] : 0;
        r->keep_on_error = keep > 0;
    }
    return err;
}

static xc_err finish(run *r)
{
    xc_err err = flush(r);
    if (err != XC_OK) {
        return err;
    }
    r->open = false;
    err = r->sink->end_file(r->sink->ctx, true);
    if (err == XC_OK && r->opts->file_done != NULL) {
        err = r->opts->file_done(r->opts->file_done_ctx, r->cur, r->crc);
    }
    if (err == XC_OK) {
        r->st.files_written++;
        r->p.files_done++;
        err = report(r);
    }
    return err;
}

/* Moves to the next file of the span that takes bytes, completing empty files on the way. */
static xc_err next_file(run *r)
{
    while (r->cur < r->end) {
        if (skipped(r, r->cur)) {
            r->cur++;
            continue;
        }
        if (!r->open) {
            xc_err err = begin(r);
            if (err != XC_OK) {
                return err;
            }
        }
        if (r->l->files[r->cur].size > 0) {
            return XC_OK;
        }
        xc_err err = finish(r);
        if (err != XC_OK) {
            return err;
        }
        r->cur++;
    }
    return XC_OK;
}

static xc_err seen(run *r, uint64_t offset, const uint8_t *data, size_t len)
{
    if (r->opts->on_bytes == NULL || len == 0) {
        return XC_OK;
    }
    return r->opts->on_bytes(r->opts->on_bytes_ctx, offset, data, len);
}

/* xc_source_data_fn: routes span bytes [offset, offset + len) to files; gaps are dropped. */
static xc_err route(void *ctx, uint64_t offset, const uint8_t *data, size_t len)
{
    run *r = ctx;
    r->st.bytes_read += len;
    while (len > 0) {
        xc_err err = next_file(r);
        if (err != XC_OK) {
            return err;
        }
        if (r->cur >= r->end) {
            return seen(r, offset, data, len); /* past the span's last file: nothing to keep */
        }
        const xc_xdvdfs_entry *f = &r->l->files[r->cur];
        uint64_t at = f->offset + r->written;
        if (offset < at) {
            /* A coalesced gap (ADR 0004 §3): read, then discarded. */
            uint64_t gap = at - offset;
            size_t n = gap < len ? (size_t)gap : len;
            err = seen(r, offset, data, n);
            if (err != XC_OK) {
                return err;
            }
            offset += n;
            data += n;
            len -= n;
            continue;
        }
        if (offset > at) {
            return XC_ERR_INVALID_ARG; /* the source skipped bytes: never write a hole */
        }
        uint32_t left = f->size - r->written;
        uint32_t every = r->opts->checkpoint_every;
        bool checkpoints =
            every > 0 && f->size > every && r->sink->sync != NULL && r->opts->checkpoint != NULL;
        if (checkpoints && every - r->written % every < left) {
            left = every - r->written % every; /* stop exactly at the next checkpoint */
        }
        size_t n = left < len ? (size_t)left : len;
        r->crc = xc_crc32(r->crc, data, n);
        err = seen(r, offset, data, n);
        if (err == XC_OK) {
            err = put(r, data, n);
        }
        if (err != XC_OK) {
            return err;
        }
        r->written += (uint32_t)n;
        r->p.bytes_done += n;
        offset += n;
        data += n;
        len -= n;
        err = report(r);
        if (err == XC_OK && r->written == f->size) {
            err = finish(r);
            r->cur++;
        } else if (err == XC_OK && checkpoints && r->written % every == 0) {
            /* ADR 0011: the bytes so far go to the disk before the journal says so. */
            err = flush(r);
            if (err == XC_OK) {
                err = r->sink->sync(r->sink->ctx);
            }
            if (err == XC_OK) {
                err = r->opts->checkpoint(r->opts->file_done_ctx, r->cur, r->written, r->crc);
                r->keep_on_error = r->keep_on_error || err == XC_OK;
            }
        }
        if (err != XC_OK) {
            return err;
        }
    }
    return XC_OK;
}

/* Reads the span [start, end) and routes it into files [first, last). */
static xc_err read_span(run *r, size_t first, size_t last, uint64_t start, uint64_t end)
{
    r->cur = first;
    r->end = last;
    r->open = false;
    r->wlen = 0;
    r->st.spans++;
    xc_err err = next_file(r); /* begin the first file before any byte arrives */
    if (err == XC_OK && r->src->read_stream != NULL) {
        r->direct = false;
        if (start > r->src->size || end - start > r->src->size - start) {
            err = XC_ERR_RANGE;
        } else {
            err = r->src->read_stream(r->src->ctx, start, end - start, route, r);
        }
    } else if (err == XC_OK) {
        r->direct = true;
        for (uint64_t pos = start; err == XC_OK && pos < end;) {
            size_t n = end - pos < r->opts->buf_size ? (size_t)(end - pos) : r->opts->buf_size;
            err = xc_source_read(r->src, pos, r->opts->buf, n);
            if (err == XC_OK) {
                err = route(r, pos, r->opts->buf, n);
            }
            pos += n;
        }
    }
    if (err == XC_OK) {
        err = next_file(r); /* empty files after the last data */
    }
    if (err == XC_OK && r->cur < r->end) {
        err = XC_ERR_IO; /* the stream ended early */
    }
    if (err != XC_OK && r->open) {
        r->open = false;
        if (r->keep_on_error && r->sink->suspend_file != NULL) {
            (void)r->sink->suspend_file(r->sink->ctx); /* ADR 0011: the next attempt continues */
        } else {
            (void)r->sink->end_file(r->sink->ctx, false);
        }
    }
    return err;
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
    r.l = listing;
    r.sink = sink;
    r.opts = opts;
    r.p.bytes_total = listing->total_file_bytes;
    r.p.file_count = listing->file_count;
    for (size_t i = 0; i < listing->file_count; i++) {
        if (skipped(&r, i)) {
            r.st.files_skipped++;
            r.p.files_done++;
            r.p.bytes_done += listing->files[i].size;
        } else {
            r.p.bytes_done += resumed(&r, i);
        }
    }

    xc_err err = XC_OK;
    /* The listing orders directories parents-first (docs/formats/xdvdfs.md §7). On a resume
       they exist already, which every sink accepts. */
    for (size_t i = 0; err == XC_OK && i < listing->dir_count; i++) {
        err = sink->make_dir(sink->ctx, listing->dirs[i].path);
        if (err == XC_OK) {
            r.st.dirs_created++;
        }
    }

    /* Whole-image read: spans start where the last one ended (from whole_from), with no gap
       limit; the image's tail after the last file is read at the end. */
    bool whole = opts->whole_image;
    uint64_t pos = opts->whole_from;
    uint64_t gap_limit = whole ? UINT64_MAX : opts->coalesce_gap;
    size_t i = 0;
    while (err == XC_OK && i < listing->file_count) {
        const xc_xdvdfs_entry *f = &listing->files[i];
        if (skipped(&r, i) || f->size == 0) {
            /* Empty files read nothing; their offset may be anywhere (never checked). */
            if (!skipped(&r, i)) {
                r.cur = i;
                r.end = i + 1;
                err = next_file(&r);
            }
            i++;
            continue;
        }
        /* A span: following files whose gap from the span's end is small. A file that starts
           before the end (overlapping data already read) starts a new span. */
        uint64_t start = f->offset + resumed(&r, i); /* only the rest of a resumed file */
        if (whole && pos <= start) {
            start = pos; /* overlapping files (start < pos) are read again on their own */
        }
        uint64_t end = f->offset + f->size;
        size_t last = i + 1;
        while (last < listing->file_count) {
            const xc_xdvdfs_entry *g = &listing->files[last];
            if (!skipped(&r, last) && g->size > 0) {
                if (g->offset < end || g->offset - end > gap_limit) {
                    break;
                }
                end = g->offset + g->size;
            }
            last++;
        }
        err = read_span(&r, i, last, start, end);
        pos = end > pos ? end : pos;
        i = last;
    }
    if (err == XC_OK && whole && pos < src->size) {
        err = read_span(&r, listing->file_count, listing->file_count, pos, src->size);
    }

    if (stats != NULL) {
        *stats = r.st;
    }
    return err;
}
