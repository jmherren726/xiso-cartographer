/*
 * One install, start to finish: docs/ARCHITECTURE.md §5, ADR 0004 (stream-extract,
 * resume), ADR 0008 (FATX rules, folder names) and docs/formats/journal.md
 * (§2 life cycle, §6 resuming, §7 verification).
 */
#include "xcat/install.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xcat/crc32.h"
#include "xcat/journal.h"
#include "xcat/version.h"
#include "xcat/xdvdfs.h"

#define MIN_BUF ((size_t)64 * 1024)
#define JOURNAL_LINE_CAP (XC_JOURNAL_MAX_LINE + 16)

/* --- a source that counts what it reads -------------------------------------------- */

typedef struct counted {
    const xc_source *inner;
    uint64_t bytes;
} counted;

static xc_err counted_read_at(void *ctx, uint64_t offset, void *buf, size_t len)
{
    counted *c = ctx;
    xc_err err = xc_source_read(c->inner, offset, buf, len);
    if (err == XC_OK) {
        c->bytes += len;
    }
    return err;
}

typedef struct counted_stream {
    counted *c;
    xc_source_data_fn fn;
    void *fn_ctx;
} counted_stream;

static xc_err counted_data(void *ctx, uint64_t offset, const uint8_t *data, size_t len)
{
    counted_stream *cs = ctx;
    cs->c->bytes += len;
    return cs->fn(cs->fn_ctx, offset, data, len);
}

static xc_err counted_read_stream(void *ctx, uint64_t offset, uint64_t len, xc_source_data_fn fn,
                                  void *fn_ctx)
{
    counted *c = ctx;
    counted_stream cs = {c, fn, fn_ctx};
    return c->inner->read_stream(c->inner->ctx, offset, len, counted_data, &cs);
}

/* --- state -------------------------------------------------------------------------- */

typedef struct job {
    const xc_install_req *req;
    const xc_install_io *io;
    xc_install_result *res;
    xc_source src; /* counted */
    counted count;
    xc_xdvdfs_volume vol;
    xc_xdvdfs_listing listing;
    xc_journal_id id;
    xc_journal_state journal;
    bool *done;
    uint32_t *crc;
    char journal_path[XC_INSTALL_MAX_PATH];
    char line[JOURNAL_LINE_CAP];
    char path[XC_INSTALL_MAX_PATH]; /* scratch */
    xc_install_progress p;
} job;

/* dir + sep + rel, with rel's '/' turned into sep. False if it doesn't fit. */
static bool join(const job *j, char *out, const char *dir, const char *rel)
{
    size_t dl = strlen(dir);
    size_t rl = strlen(rel);
    if (dl + 1 + rl + 1 > XC_INSTALL_MAX_PATH) {
        return false;
    }
    memcpy(out, dir, dl);
    out[dl] = j->io->separator;
    for (size_t i = 0; i < rl; i++) {
        out[dl + 1 + i] = rel[i] == '/' ? j->io->separator : rel[i];
    }
    out[dl + 1 + rl] = '\0';
    return true;
}

static bool report(job *j)
{
    return j->req->progress == NULL || j->req->progress(j->req->progress_ctx, &j->p);
}

/* --- §6: choosing or resuming the folder -------------------------------------------- */

static xc_err choose_folder(job *j)
{
    const xc_install_io *io = j->io;
    char base[XC_FATX_MAX_NAME + 1];
    char name[XC_FATX_MAX_NAME + 1];
    (void)xc_fatx_title_name(j->req->title != NULL ? j->req->title : "", base, XC_FATX_MAX_NAME);
    for (unsigned k = 1; k <= XC_INSTALL_MAX_NUMBERED; k++) {
        if (k == 1) {
            memcpy(name, base, strlen(base) + 1);
        } else {
            (void)xc_fatx_numbered_name(base, k, name);
        }
        char *dir = j->res->folder;
        if (!join(j, dir, j->req->games_dir, name) ||
            !join(j, j->journal_path, dir, XC_JOURNAL_NAME) ||
            !join(j, j->path, dir, XC_INSTALL_MARKER)) {
            return XC_ERR_LIMIT;
        }
        if (!io->exists(io->ctx, dir)) {
            return XC_OK; /* a fresh folder */
        }
        if (io->exists(io->ctx, j->path) || !io->exists(io->ctx, j->journal_path)) {
            /* A finished install, or a folder that isn't ours: never touched (ADR 0008). */
            if (j->res->other_folder_reason == NULL) {
                j->res->other_folder_reason = io->exists(io->ctx, j->path)
                                                  ? "already installed there"
                                                  : "a folder of that name exists";
            }
            continue;
        }
        char *data = NULL;
        size_t len = 0;
        xc_err err = io->read_all(io->ctx, j->journal_path, XC_JOURNAL_MAX_FILE, &data, &len);
        if (err == XC_ERR_NOMEM) {
            return err;
        }
        if (err == XC_OK) {
            xc_journal_state_free(&j->journal);
            err = xc_journal_read(data, len, &j->listing, &j->id, &j->journal);
            free(data);
            if (err != XC_OK) {
                return err;
            }
            if (j->journal.valid) {
                j->res->resumed = true;
                return XC_OK;
            }
            if (j->res->other_folder_reason == NULL) {
                j->res->other_folder_reason = j->journal.why;
            }
        } else if (j->res->other_folder_reason == NULL) {
            j->res->other_folder_reason = "its journal can't be read";
        }
    }
    return XC_ERR_FOLDER;
}

/* §6.5: a done file must still be there with its size, or it's written again. */
static void check_done_files(job *j)
{
    for (size_t i = 0; i < j->listing.file_count; i++) {
        if (!j->done[i]) {
            continue;
        }
        uint64_t size = 0;
        if (!join(j, j->path, j->res->folder, j->listing.files[i].path) ||
            j->io->file_size(j->io->ctx, j->path, &size) != XC_OK ||
            size != j->listing.files[i].size) {
            j->done[i] = false;
        }
    }
}

/* --- extraction callbacks ----------------------------------------------------------- */

static bool on_extract_progress(void *ctx, const xc_extract_progress *ep)
{
    job *j = ctx;
    j->p.phase = XC_INSTALL_WRITING;
    j->p.bytes_done = ep->bytes_done;
    j->p.bytes_total = ep->bytes_total;
    j->p.files_done = ep->files_done;
    j->p.file_count = ep->file_count;
    j->p.current = ep->current;
    return report(j);
}

/* journal.md §2.3: the sink made the file durable; now record it, durably. */
static xc_err on_file_done(void *ctx, size_t index, uint32_t crc)
{
    job *j = ctx;
    const xc_xdvdfs_entry *f = &j->listing.files[index];
    j->crc[index] = crc;
    j->done[index] = true;
    size_t len = 0;
    xc_err err = xc_journal_format_done(f->path, f->size, crc, j->line, sizeof j->line, &len);
    if (err == XC_OK) {
        err = j->io->append(j->io->ctx, j->journal_path, j->line, len);
    }
    return err;
}

/* --- §7: read-back verification ----------------------------------------------------- */

static xc_err verify(job *j)
{
    const xc_install_io *io = j->io;
    j->p.phase = XC_INSTALL_VERIFYING;
    j->p.bytes_done = 0;
    j->p.bytes_total = j->listing.total_file_bytes;
    j->p.files_done = 0;
    for (size_t i = 0; i < j->listing.file_count; i++) {
        const xc_xdvdfs_entry *f = &j->listing.files[i];
        j->p.current = f->path;
        if (!join(j, j->path, j->res->folder, f->path)) {
            return XC_ERR_LIMIT;
        }
        xc_source src;
        void *h = NULL;
        xc_err err = io->open_file(io->ctx, j->path, &src, &h);
        bool ok = err == XC_OK && src.size == f->size;
        uint32_t crc = 0;
        for (uint64_t pos = 0; ok && pos < f->size;) {
            size_t n =
                f->size - pos < j->req->buf_size ? (size_t)(f->size - pos) : j->req->buf_size;
            err = xc_source_read(&src, pos, j->req->buf, n);
            ok = err == XC_OK;
            if (ok) {
                crc = xc_crc32(crc, j->req->buf, n);
                pos += n;
                j->p.bytes_done += n;
                if (!report(j)) {
                    io->close_file(io->ctx, h);
                    return XC_ERR_CANCELLED;
                }
            }
        }
        if (h != NULL) {
            io->close_file(io->ctx, h);
        }
        if (!ok || crc != j->crc[i]) {
            (void)snprintf(j->res->bad_file, sizeof j->res->bad_file, "%s", f->path);
            /* Deleting it makes the next attempt write it again (§6.5); the journal stays. */
            (void)io->remove(io->ctx, j->path);
            return err == XC_ERR_NOMEM ? err : XC_ERR_VERIFY;
        }
        j->p.files_done++;
    }
    return XC_OK;
}

/* --- the install ------------------------------------------------------------------- */

static xc_err write_marker(job *j)
{
    char text[XC_INSTALL_MAX_PATH + 256];
    int n = snprintf(text, sizeof text,
                     "xiso-cartographer %s\nimage=%s\nfiles=%zu\nbytes=%llu\nverified=crc32\n",
                     xc_version_string(), j->req->source_name != NULL ? j->req->source_name : "",
                     j->listing.file_count, (unsigned long long)j->listing.total_file_bytes);
    if (n < 0 || !join(j, j->path, j->res->folder, XC_INSTALL_MARKER)) {
        return XC_ERR_LIMIT;
    }
    size_t len = (size_t)n < sizeof text ? (size_t)n : sizeof text - 1;
    return j->io->write_all(j->io->ctx, j->path, text, len);
}

static uint64_t round_up(uint64_t v, uint32_t cluster)
{
    return cluster == 0 ? v : (v + cluster - 1U) / cluster * cluster;
}

static xc_err run_install(job *j)
{
    const xc_install_req *req = j->req;
    const xc_install_io *io = j->io;
    xc_install_result *res = j->res;

    j->p.phase = XC_INSTALL_READING_TABLES;
    if (!report(j)) {
        return XC_ERR_CANCELLED;
    }
    xc_err err = xc_xdvdfs_open(&j->src, &j->vol);
    if (err == XC_OK) {
        err = xc_xdvdfs_list(&j->src, &j->vol, NULL, &j->listing);
    }
    if (err != XC_OK) {
        return err;
    }
    res->file_count = j->listing.file_count;
    size_t files = j->listing.file_count > 0 ? j->listing.file_count : 1;
    j->done = calloc(files, sizeof *j->done);
    j->crc = calloc(files, sizeof *j->crc);
    if (j->done == NULL || j->crc == NULL) {
        return XC_ERR_NOMEM;
    }
    xc_journal_make_id(&j->id, &j->listing, j->vol.partition_offset, j->src.size, req->etag,
                       req->last_modified);

    err = choose_folder(j);
    if (err != XC_OK) {
        return err;
    }
    /* ADR 0008: the paths are what they'd be under \Games\<folder> on the console. */
    const char *name = strrchr(res->folder, io->separator);
    name = name != NULL ? name + 1 : res->folder;
    err = xc_fatx_check_listing(&j->listing, strlen("\\Games\\") + strlen(name), res->problems,
                                XC_INSTALL_MAX_PROBLEMS, &res->problem_count);
    if (err == XC_OK && res->problem_count > 0) {
        for (size_t i = 0; i < res->problem_count && i < XC_INSTALL_MAX_PROBLEMS; i++) {
            (void)snprintf(res->problem_paths[i], sizeof res->problem_paths[i], "%s",
                           res->problems[i].path);
            res->problems[i].path = res->problem_paths[i];
        }
        err = XC_ERR_NOT_INSTALLABLE;
    }
    if (err != XC_OK) {
        return err;
    }
    if (res->resumed) {
        for (size_t i = 0; i < j->listing.file_count; i++) {
            j->done[i] = j->journal.done[i];
            j->crc[i] = j->journal.crc[i];
        }
        check_done_files(j);
    }

    /* Free space for everything still to be written, cluster-rounded (as M2 did). */
    uint64_t need = (uint64_t)(j->listing.dir_count + 2U) * req->cluster_size;
    for (size_t i = 0; i < j->listing.file_count; i++) {
        if (!j->done[i]) {
            need += round_up(j->listing.files[i].size, req->cluster_size);
        }
    }
    uint64_t free_bytes = 0;
    err = io->free_space(io->ctx, req->games_dir, &free_bytes);
    if (err != XC_OK) {
        return err;
    }
    if (free_bytes < need) {
        res->space_needed = need;
        res->space_free = free_bytes;
        return XC_ERR_NO_SPACE;
    }

    xc_sink sink;
    void *sink_handle = NULL;
    err = io->open_sink(io->ctx, res->folder, &sink, &sink_handle);
    if (err != XC_OK) {
        return err;
    }
    if (!res->resumed) {
        /* journal.md §2.2: the journal comes before any game file. */
        size_t len = 0;
        char *start = malloc((size_t)3 * XC_JOURNAL_MAX_LINE);
        err = start != NULL ? xc_journal_format_start(&j->id, req->source_name, start,
                                                      (size_t)3 * XC_JOURNAL_MAX_LINE, &len)
                            : XC_ERR_NOMEM;
        if (err == XC_OK) {
            err = io->append(io->ctx, j->journal_path, start, len);
        }
        free(start);
    }
    if (err == XC_OK) {
        xc_extract_opts o;
        memset(&o, 0, sizeof o);
        o.buf = req->buf;
        o.buf_size = req->buf_size;
        o.coalesce_gap = req->coalesce_gap != 0 ? req->coalesce_gap : XC_EXTRACT_DEFAULT_GAP;
        o.progress = on_extract_progress;
        o.progress_ctx = j;
        o.file_done = on_file_done;
        o.file_done_ctx = j;
        /* `skip` must not change while extracting: on_file_done marks files in a copy. */
        bool *skip = malloc(files * sizeof *skip);
        if (skip == NULL) {
            err = XC_ERR_NOMEM;
        } else {
            memcpy(skip, j->done, files * sizeof *skip);
            o.skip = skip;
            xc_extract_stats st;
            err = xc_extract(&j->src, &j->listing, &sink, &o, &st);
            res->files_skipped = st.files_skipped;
            res->files_written = st.files_written;
            res->bytes_written = st.bytes_written;
            free(skip);
        }
    }
    io->close_sink(io->ctx, sink_handle);
    if (err == XC_OK) {
        err = verify(j);
    }
    if (err == XC_OK) {
        err = write_marker(j);
    }
    if (err == XC_OK) {
        (void)io->remove(io->ctx, j->journal_path); /* §2.4: finished */
    }
    return err;
}

xc_err xc_install(const xc_install_req *req, xc_install_result *res)
{
    if (res != NULL) {
        memset(res, 0, sizeof *res);
    }
    if (req == NULL || res == NULL || req->src == NULL || req->io == NULL ||
        req->games_dir == NULL || req->buf == NULL || req->buf_size < MIN_BUF) {
        return XC_ERR_INVALID_ARG;
    }
    job *j = calloc(1, sizeof *j);
    if (j == NULL) {
        return XC_ERR_NOMEM;
    }
    j->req = req;
    j->io = req->io;
    j->res = res;
    j->count.inner = req->src;
    j->src.read_at = counted_read_at;
    j->src.read_stream = req->src->read_stream != NULL ? counted_read_stream : NULL;
    j->src.ctx = &j->count;
    j->src.size = req->src->size;
    xc_err err = run_install(j);
    res->bytes_read = j->count.bytes;
    xc_journal_state_free(&j->journal);
    xc_xdvdfs_listing_free(&j->listing);
    free(j->done);
    free(j->crc);
    free(j);
    return err;
}
