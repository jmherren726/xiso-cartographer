/*
 * One install, start to finish: docs/ARCHITECTURE.md §5, ADR 0004 (stream-extract,
 * resume), ADR 0008 (FATX rules, folder names) and docs/formats/journal.md
 * (§2 life cycle, §6 resuming, §7 verification and whole-image hashes).
 */
#include "xcat/install.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xcat/crc32.h"
#include "xcat/hash.h"
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
    uint32_t *resume_bytes; /* ADR 0011: checkpoints to continue from */
    uint32_t *resume_crc;
    char journal_path[XC_INSTALL_MAX_PATH];
    char line[JOURNAL_LINE_CAP];
    char path[XC_INSTALL_MAX_PATH]; /* scratch */
    xc_install_progress p;
    /* journal.md §7: the whole-image hash, fed by the extractor's on_bytes */
    xc_image_hash *hash;      /* NULL: not hashing */
    bool hash_broken;         /* bytes came out of order (overlapping files) */
    uint64_t hash_next;       /* the image offset the next byte must have */
    uint64_t hash_saved;      /* offset of the last `hash` line written */
    uint32_t hash_save_every; /* image bytes between `hash` lines */
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

/* journal.md §7: every image byte, in order, goes through the hash. */
static xc_err on_bytes(void *ctx, uint64_t offset, const uint8_t *data, size_t len)
{
    job *j = ctx;
    if (j->hash == NULL || j->hash_broken) {
        return XC_OK;
    }
    if (offset != j->hash_next) {
        j->hash_broken = true; /* files overlap: a span went back (journal.md §7) */
        return XC_OK;
    }
    xc_image_hash_update(j->hash, data, len);
    j->hash_next += len;
    return XC_OK;
}

/* journal.md §5: after a done or part line, a `hash` line once 64 MiB more were hashed. */
static xc_err save_hash(job *j)
{
    if (j->hash == NULL || j->hash_broken || j->hash_next - j->hash_saved < j->hash_save_every) {
        return XC_OK;
    }
    uint8_t md5[XC_MD5_BYTES];
    uint8_t sha1[XC_SHA1_BYTES];
    uint64_t off = xc_image_hash_save(j->hash, md5, sha1);
    size_t len = 0;
    xc_err err = xc_journal_format_hash(off, md5, sha1, j->line, sizeof j->line, &len);
    if (err == XC_OK) {
        err = j->io->append(j->io->ctx, j->journal_path, j->line, len);
    }
    if (err == XC_OK) {
        j->hash_saved = off;
    }
    return err;
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
    return err == XC_OK ? save_hash(j) : err;
}

/* ADR 0011: the sink flushed the first `bytes` bytes; record the checkpoint, durably. */
static xc_err on_checkpoint(void *ctx, size_t index, uint32_t bytes, uint32_t crc)
{
    job *j = ctx;
    size_t len = 0;
    xc_err err = xc_journal_format_part(j->listing.files[index].path, bytes, crc, j->line,
                                        sizeof j->line, &len);
    if (err == XC_OK) {
        err = j->io->append(j->io->ctx, j->journal_path, j->line, len);
    }
    return err == XC_OK ? save_hash(j) : err;
}

/*
 * journal.md §6.7 and §7: whether to hash, and from where. Returns the image offset the
 * whole-image read starts at. A journal's hash state is used only if nothing still to be
 * written starts before it; otherwise hashing starts again at byte 0.
 */
static xc_err start_hash(job *j, uint64_t *from)
{
    *from = 0;
    if (j->req->no_hash) {
        j->res->hash_note = "not asked for";
        return XC_OK;
    }
    if (j->vol.layout != XC_XDVDFS_XISO) {
        j->res->hash_note = "full-disc layout: checked by size and CRC-32 only (ADR 0004 §5)";
        return XC_OK;
    }
    xc_err err = xc_image_hash_create(&j->hash);
    if (err != XC_OK) {
        return err;
    }
    if (!j->res->resumed) {
        return XC_OK;
    }
    uint64_t first_needed = UINT64_MAX;
    bool any_kept = false;
    for (size_t i = 0; i < j->listing.file_count; i++) {
        const xc_xdvdfs_entry *f = &j->listing.files[i];
        any_kept = any_kept || j->done[i] || j->resume_bytes[i] > 0;
        if (!j->done[i] && f->size > 0 && f->offset + j->resume_bytes[i] < first_needed) {
            first_needed = f->offset + j->resume_bytes[i];
        }
    }
    if (j->journal.has_hash && j->journal.hash_offset <= first_needed) {
        err = xc_image_hash_restore(j->hash, j->journal.hash_offset, j->journal.hash_md5,
                                    j->journal.hash_sha1);
        if (err == XC_OK) {
            *from = j->journal.hash_offset;
        }
        return err;
    }
    j->res->hash_restarted = any_kept;
    return XC_OK;
}

/* journal.md §7: the result, and the comparison with the expected values. */
static xc_err finish_hash(job *j)
{
    if (j->hash == NULL) {
        return XC_OK;
    }
    if (j->hash_broken || j->hash_next != j->src.size) {
        j->res->hash_note = "files overlap in the image, so it couldn't be read front to back";
        return XC_OK;
    }
    xc_image_hash_final(j->hash, j->res->md5, j->res->sha1);
    bool md5_bad =
        j->req->expect_md5 != NULL && memcmp(j->req->expect_md5, j->res->md5, XC_MD5_BYTES) != 0;
    bool sha1_bad = j->req->expect_sha1 != NULL &&
                    memcmp(j->req->expect_sha1, j->res->sha1, XC_SHA1_BYTES) != 0;
    j->res->hash = j->req->expect_md5 != NULL || j->req->expect_sha1 != NULL
                       ? XC_INSTALL_HASH_MATCHED
                       : XC_INSTALL_HASH_COMPUTED;
    if (md5_bad || sha1_bad) {
        j->res->hash = XC_INSTALL_HASH_COMPUTED;
        return XC_ERR_IMAGE_HASH;
    }
    return XC_OK;
}

/* journal.md §6.6: an unfinished file continues at its last checkpoint if the file on disk is
   at least that long; otherwise it's written from the start. */
static void check_checkpoints(job *j)
{
    for (size_t i = 0; i < j->listing.file_count; i++) {
        uint32_t keep = j->journal.part_bytes[i];
        uint64_t size = 0;
        if (j->done[i] || keep == 0 ||
            !join(j, j->path, j->res->folder, j->listing.files[i].path) ||
            j->io->file_size(j->io->ctx, j->path, &size) != XC_OK || size < keep) {
            continue;
        }
        j->resume_bytes[i] = keep;
        j->resume_crc[i] = j->journal.part_crc[i];
        j->res->files_continued++;
        j->res->bytes_continued += keep;
    }
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
    char text[XC_INSTALL_MAX_PATH + 384];
    char md5[2 * XC_MD5_BYTES + 1];
    char sha1[2 * XC_SHA1_BYTES + 1];
    xc_hex_lower(j->res->md5, XC_MD5_BYTES, md5);
    xc_hex_lower(j->res->sha1, XC_SHA1_BYTES, sha1);
    bool hashed = j->res->hash != XC_INSTALL_HASH_NONE;
    int n =
        snprintf(text, sizeof text,
                 "xiso-cartographer %s\nimage=%s\nfiles=%zu\nbytes=%llu\nverified=%s\n%s%s%s%s%s",
                 xc_version_string(), j->req->source_name != NULL ? j->req->source_name : "",
                 j->listing.file_count, (unsigned long long)j->listing.total_file_bytes,
                 j->res->hash == XC_INSTALL_HASH_MATCHED ? "crc32,image-hash" : "crc32",
                 hashed ? "md5=" : "", hashed ? md5 : "", hashed ? "\nsha1=" : "",
                 hashed ? sha1 : "", hashed ? "\n" : "");
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
    j->resume_bytes = calloc(files, sizeof *j->resume_bytes);
    j->resume_crc = calloc(files, sizeof *j->resume_crc);
    if (j->done == NULL || j->crc == NULL || j->resume_bytes == NULL || j->resume_crc == NULL) {
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
        check_checkpoints(j);
    }

    /* Free space for everything still to be written, cluster-rounded (as M2 did). */
    uint64_t need = (uint64_t)(j->listing.dir_count + 2U) * req->cluster_size;
    for (size_t i = 0; i < j->listing.file_count; i++) {
        if (!j->done[i]) {
            need += round_up(j->listing.files[i].size - j->resume_bytes[i], req->cluster_size);
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
        o.checkpoint_every =
            req->checkpoint_bytes != 0 ? req->checkpoint_bytes : XC_INSTALL_CHECKPOINT;
        o.checkpoint = on_checkpoint;
        o.resume_bytes = j->resume_bytes;
        o.resume_crc = j->resume_crc;
        uint64_t from = 0;
        err = start_hash(j, &from);
        j->hash_next = from;
        j->hash_saved = from;
        j->hash_save_every = o.checkpoint_every;
        o.whole_image = j->hash != NULL;
        o.whole_from = from;
        o.on_bytes = on_bytes;
        o.on_bytes_ctx = j;
        /* `skip` must not change while extracting: on_file_done marks files in a copy. */
        bool *skip = err == XC_OK ? malloc(files * sizeof *skip) : NULL;
        if (skip == NULL) {
            err = err == XC_OK ? XC_ERR_NOMEM : err;
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
        err = finish_hash(j);
    }
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
    if (err != XC_OK && j->p.current != NULL) {
        (void)snprintf(res->current_file, sizeof res->current_file, "%s", j->p.current);
    }
    xc_journal_state_free(&j->journal);
    xc_xdvdfs_listing_free(&j->listing);
    free(j->done);
    free(j->crc);
    free(j->resume_bytes);
    free(j->resume_crc);
    xc_image_hash_free(j->hash);
    free(j);
    return err;
}
