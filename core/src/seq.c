/*
 * Sequential extraction: docs/formats/stream-install.md. One front-to-back pass over the
 * image (more if the spool had to drop bytes that turned out to be needed).
 */
#include "xcat/seq.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xcat/crc32.h"

#define SECTOR 2048U
#define VD_OFFSET 0x10000U
#define MAX_TABLE_READ (0x40000U + SECTOR) /* xdvdfs.md §3 rule 1 */
#define MAX_CHUNKS (1U << 22)
#define PROGRESS_STEP ((uint64_t)1024 * 1024)
#define NONE SIZE_MAX

/* Candidate partitions in stream order (§2.1): xiso, then XGD3, XGD2, XGD1. */
static const uint64_t candidates[] = {0, 0x02080000U, 0x0FD90000U, 0x18300000U};
#define NCANDIDATES (sizeof candidates / sizeof candidates[0])

/* --- small containers -------------------------------------------------------------- */

typedef struct pathset {
    char **v; /* sorted */
    size_t n, cap;
} pathset;

static size_t pathset_find(const pathset *s, const char *key, bool *found)
{
    size_t lo = 0;
    size_t hi = s->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(s->v[mid], key);
        if (c == 0) {
            *found = true;
            return mid;
        }
        if (c < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    *found = false;
    return lo;
}

static bool pathset_has(const pathset *s, const char *key)
{
    bool found = false;
    (void)pathset_find(s, key, &found);
    return found;
}

static xc_err pathset_add(pathset *s, const char *key)
{
    bool found = false;
    size_t at = pathset_find(s, key, &found);
    if (found) {
        return XC_OK;
    }
    if (s->n == s->cap) {
        size_t cap = s->cap > 0 ? s->cap * 2 : 64;
        char **v = (char **)realloc((void *)s->v, cap * sizeof *v);
        if (v == NULL) {
            return XC_ERR_NOMEM;
        }
        s->v = v;
        s->cap = cap;
    }
    size_t len = strlen(key) + 1;
    char *copy = malloc(len);
    if (copy == NULL) {
        return XC_ERR_NOMEM;
    }
    memcpy(copy, key, len);
    memmove((void *)(s->v + at + 1), (const void *)(s->v + at), (s->n - at) * sizeof *s->v);
    s->v[at] = copy;
    s->n++;
    return XC_OK;
}

static void pathset_free(pathset *s)
{
    for (size_t i = 0; i < s->n; i++) {
        free(s->v[i]);
    }
    free((void *)s->v);
    memset(s, 0, sizeof *s);
}

/* --- state ------------------------------------------------------------------------- */

typedef struct table {
    uint64_t off;
    size_t len;    /* bytes the reader needs: min(size, MAX_TABLE_READ) */
    uint8_t *data; /* len bytes */
    uint8_t *got;  /* one flag per byte while incomplete; NULL once complete */
    size_t have;   /* bytes captured */
} table;

typedef struct spool_span {
    uint64_t img; /* image offset of the first byte */
    uint64_t len;
    uint32_t chunk;
    uint64_t pos; /* where in the chunk */
} spool_span;

typedef enum file_state { F_TODO, F_DIRECT, F_LATE, F_DONE } file_state;

typedef struct seq {
    const xc_seq_req *req;
    xc_seq_result *res; /* never NULL */
    xc_err err;
    bool stop; /* nothing more is needed from this pass's stream */

    /* volume detection (§2) */
    bool vol_known;
    size_t cand;
    uint8_t vd[SECTOR];
    uint8_t *pre; /* [partition, descriptor) of the current candidate */

    /* directory tables */
    table *tables; /* sorted by offset */
    size_t ntables, cap_tables;
    bool all_known;
    xc_xdvdfs_listing listing;
    file_state *state; /* per listing file */
    pathset done;
    pathset made; /* directories created */

    /* writing */
    uint64_t pos;    /* the next stream byte */
    size_t dcur;     /* listing index of the next direct file, or NONE */
    size_t writing;  /* listing index of the open file, or NONE */
    uint64_t wdone;  /* bytes of it written (incl. buffered) */
    size_t buffered; /* in req->buf */
    uint32_t crc;

    /* spool */
    spool_span *spans; /* sorted by img */
    size_t nspans, cap_spans;
    uint64_t *chunk_used; /* bytes in each chunk file; 0 = no file */
    size_t nchunks;
    uint64_t spool_bytes;
    uint64_t free_est;
    /* resolve(): make_room() keeps chunks [keep_lo, keep_hi] (the file being copied) and
       drops the others from the top down, so the next pass starts as late as possible */
    bool copying;
    uint32_t keep_lo, keep_hi;

    unsigned pass;
    uint64_t next_progress;
} seq;

static uint64_t chunk_bytes(const seq *s)
{
    return s->req->chunk_bytes > 0 ? s->req->chunk_bytes : XC_SEQ_DEFAULT_CHUNK;
}

static uint64_t rounded(const seq *s, uint64_t n)
{
    uint64_t c = s->req->cluster_size;
    return c > 0 ? (n + c - 1) / c * c : n;
}

static void note_file(seq *s, const char *path)
{
    (void)snprintf(s->res->current_file, sizeof s->res->current_file, "%s",
                   path != NULL ? path : "");
}

static bool progress(seq *s, bool force)
{
    if (s->req->progress == NULL || (!force && s->pos < s->next_progress)) {
        return true;
    }
    s->next_progress = s->pos + PROGRESS_STEP;
    xc_seq_progress p;
    p.pass = s->pass;
    p.offset = s->pos;
    p.image_size = s->req->image_size;
    p.spool_bytes = s->spool_bytes;
    p.files_done = s->done.n; /* every path in it is a finished file */
    p.file_count = s->listing.file_count;
    p.all_known = s->all_known;
    p.current = s->writing != NONE ? s->listing.files[s->writing].path : NULL;
    if (!s->req->progress(s->req->ctx, &p)) {
        s->err = XC_ERR_CANCELLED;
        return false;
    }
    return true;
}

/* --- tables ---------------------------------------------------------------------- */

static table *find_table(seq *s, uint64_t off)
{
    size_t lo = 0;
    size_t hi = s->ntables;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (s->tables[mid].off == off) {
            return &s->tables[mid];
        }
        if (s->tables[mid].off < off) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return NULL;
}

static bool have_table(void *ctx, uint64_t offset, size_t len)
{
    const table *t = find_table(ctx, offset);
    return t != NULL && t->got == NULL && t->len >= len;
}

static xc_err read_table(void *ctx, uint64_t offset, void *buf, size_t len)
{
    const table *t = find_table(ctx, offset);
    if (t == NULL || t->got != NULL || t->len < len) {
        return XC_ERR_RANGE;
    }
    memcpy(buf, t->data, len);
    return XC_OK;
}

/* Adds a wanted table (if new); *added tells whether it was. */
static xc_err want_table(seq *s, uint64_t off, uint64_t size, bool *added)
{
    *added = false;
    if (size == 0 || find_table(s, off) != NULL) {
        return XC_OK;
    }
    if (s->ntables == s->cap_tables) {
        size_t cap = s->cap_tables > 0 ? s->cap_tables * 2 : 16;
        table *t = realloc(s->tables, cap * sizeof *t);
        if (t == NULL) {
            return XC_ERR_NOMEM;
        }
        s->tables = t;
        s->cap_tables = cap;
    }
    table t;
    t.off = off;
    t.len = (size_t)(size < MAX_TABLE_READ ? size : MAX_TABLE_READ);
    t.data = malloc(t.len);
    t.got = calloc(t.len, 1);
    t.have = 0;
    if (t.data == NULL || t.got == NULL) {
        free(t.data);
        free(t.got);
        return XC_ERR_NOMEM;
    }
    size_t at = 0;
    while (at < s->ntables && s->tables[at].off < off) {
        at++;
    }
    memmove(s->tables + at + 1, s->tables + at, (s->ntables - at) * sizeof *s->tables);
    s->tables[at] = t;
    s->ntables++;
    *added = true;
    return XC_OK;
}

/* Copies image bytes [off, off + n) into every incomplete table they overlap. */
static bool capture(seq *s, uint64_t off, const uint8_t *data, uint64_t n)
{
    bool completed = false;
    for (size_t i = 0; i < s->ntables; i++) {
        table *t = &s->tables[i];
        if (t->got == NULL || t->off >= off + n || t->off + t->len <= off) {
            continue;
        }
        uint64_t a = off > t->off ? off : t->off;
        uint64_t b = off + n < t->off + t->len ? off + n : t->off + t->len;
        for (uint64_t p = a; p < b; p++) {
            size_t k = (size_t)(p - t->off);
            if (!t->got[k]) {
                t->got[k] = 1;
                t->data[k] = data[p - off];
                t->have++;
            }
        }
        if (t->have == t->len) {
            free(t->got);
            t->got = NULL;
            completed = true;
        }
    }
    return completed;
}

/* The next byte at or after `off` that some incomplete table wants, or UINT64_MAX. */
static uint64_t next_wanted(const seq *s, uint64_t off, bool *inside)
{
    uint64_t best = UINT64_MAX;
    *inside = false;
    for (size_t i = 0; i < s->ntables; i++) {
        const table *t = &s->tables[i];
        if (t->got == NULL || t->off + t->len <= off) {
            continue;
        }
        if (t->off <= off) {
            *inside = true;
            return off;
        }
        if (t->off < best) {
            best = t->off;
        }
    }
    return best;
}

/* --- spool ------------------------------------------------------------------------- */

/* Index of the first span ending after `img`. */
static size_t span_at(const seq *s, uint64_t img)
{
    size_t lo = 0;
    size_t hi = s->nspans;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (s->spans[mid].img + s->spans[mid].len <= img) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/* True if every byte of [off, off + n) is in the spool. */
static bool spooled(const seq *s, uint64_t off, uint64_t n)
{
    uint64_t p = off;
    for (size_t i = span_at(s, off); i < s->nspans && p < off + n; i++) {
        if (s->spans[i].img > p) {
            return false;
        }
        p = s->spans[i].img + s->spans[i].len;
    }
    return p >= off + n;
}

/* Reads [off, off + n) from the spool (every byte must be there). */
static xc_err spool_read(seq *s, uint64_t off, uint8_t *buf, size_t n)
{
    uint64_t p = off;
    for (size_t i = span_at(s, off); i < s->nspans && p < off + n; i++) {
        const spool_span *sp = &s->spans[i];
        if (sp->img > p) {
            return XC_ERR_RANGE;
        }
        uint64_t end = sp->img + sp->len < off + n ? sp->img + sp->len : off + n;
        xc_err err = s->req->spool->read(s->req->spool->ctx, sp->chunk, sp->pos + (p - sp->img),
                                         buf + (p - off), (size_t)(end - p));
        if (err != XC_OK) {
            return err;
        }
        p = end;
    }
    return p >= off + n ? XC_OK : XC_ERR_RANGE;
}

/* Fills incomplete tables from the spool where it has their bytes. */
static bool fill_tables_from_spool(seq *s)
{
    bool completed = false;
    for (size_t i = 0; i < s->ntables && s->err == XC_OK; i++) {
        table *t = &s->tables[i];
        if (t->got == NULL) {
            continue;
        }
        for (size_t j = span_at(s, t->off); j < s->nspans; j++) {
            const spool_span *sp = &s->spans[j];
            if (sp->img >= t->off + t->len) {
                break;
            }
            uint64_t a = sp->img > t->off ? sp->img : t->off;
            uint64_t b = sp->img + sp->len < t->off + t->len ? sp->img + sp->len : t->off + t->len;
            uint8_t *tmp = s->req->buf;
            for (uint64_t p = a; p < b;) {
                size_t n = (size_t)(b - p < s->req->buf_size ? b - p : s->req->buf_size);
                xc_err err = spool_read(s, p, tmp, n);
                if (err != XC_OK) {
                    s->err = err;
                    return false;
                }
                completed |= capture(s, p, tmp, n);
                p += n;
            }
            if (t->got == NULL) {
                break;
            }
        }
    }
    return completed;
}

/* Deletes chunk `c`: its spans go, its space comes back. */
static xc_err drop_chunk(seq *s, uint32_t c)
{
    if (s->chunk_used[c] == 0) {
        return XC_OK;
    }
    xc_err err = s->req->spool->drop(s->req->spool->ctx, c);
    if (err != XC_OK) {
        return err;
    }
    size_t k = 0;
    for (size_t i = 0; i < s->nspans; i++) {
        if (s->spans[i].chunk != c) {
            s->spans[k++] = s->spans[i];
        }
    }
    s->nspans = k;
    s->free_est += s->chunk_used[c];
    s->spool_bytes -= s->chunk_used[c];
    s->chunk_used[c] = 0;
    return XC_OK;
}

/*
 * Makes room for `need` more bytes above the reserve, dropping the oldest spool chunks
 * (§4.2). False if it can't: the spool is empty and there still isn't enough.
 */
static bool make_room(seq *s, uint64_t need)
{
    if (s->free_est >= s->req->reserve && s->free_est - s->req->reserve >= need) {
        return true;
    }
    uint64_t fresh = 0;
    xc_err err = s->req->spool->free_space(s->req->spool->ctx, &fresh);
    if (err != XC_OK) {
        s->err = err;
        return false;
    }
    s->free_est = fresh;
    for (size_t k = 0; k < s->nchunks; k++) {
        uint32_t c = (uint32_t)(s->copying ? s->nchunks - 1 - k : k);
        if (s->free_est >= s->req->reserve && s->free_est - s->req->reserve >= need) {
            return true;
        }
        if (s->copying && c >= s->keep_lo && c <= s->keep_hi) {
            continue;
        }
        if (s->chunk_used[c] > 0) {
            uint64_t used = s->chunk_used[c];
            err = drop_chunk(s, c);
            if (err != XC_OK) {
                s->err = err;
                return false;
            }
            s->res->spool_dropped += used;
        }
    }
    return s->free_est >= s->req->reserve && s->free_est - s->req->reserve >= need;
}

/* Spools unidentified bytes [off, off + n), skipping any already in the spool. */
static void spool_put(seq *s, uint64_t off, const uint8_t *data, uint64_t n)
{
    uint64_t end = off + n;
    while (off < end && s->err == XC_OK) {
        /* skip what's already there (a later pass over spooled bytes) */
        size_t i = span_at(s, off);
        if (i < s->nspans && s->spans[i].img <= off) {
            uint64_t skip = s->spans[i].img + s->spans[i].len - off;
            skip = skip < end - off ? skip : end - off;
            data += skip;
            off += skip;
            continue;
        }
        uint64_t piece = end - off;
        if (i < s->nspans && s->spans[i].img < end) {
            piece = s->spans[i].img - off;
        }
        uint64_t cb = chunk_bytes(s);
        uint64_t to_chunk_end = cb - off % cb;
        piece = piece < to_chunk_end ? piece : to_chunk_end;
        uint32_t c = (uint32_t)(off / cb);
        if (!make_room(s, piece)) {
            if (s->err != XC_OK) {
                return;
            }
            data += piece; /* no room even with an empty spool: these bytes are lost */
            off += piece;
            continue;
        }
        xc_err err = s->req->spool->append(s->req->spool->ctx, c, data, (size_t)piece);
        if (err != XC_OK) {
            s->err = err;
            return;
        }
        /* make_room may have dropped spans: find the place again */
        i = span_at(s, off);
        spool_span sp = {off, piece, c, s->chunk_used[c]};
        if (i > 0 && s->spans[i - 1].chunk == c &&
            s->spans[i - 1].img + s->spans[i - 1].len == off &&
            s->spans[i - 1].pos + s->spans[i - 1].len == sp.pos) {
            s->spans[i - 1].len += piece;
        } else {
            if (s->nspans == s->cap_spans) {
                size_t cap = s->cap_spans > 0 ? s->cap_spans * 2 : 256;
                spool_span *v = realloc(s->spans, cap * sizeof *v);
                if (v == NULL) {
                    s->err = XC_ERR_NOMEM;
                    return;
                }
                s->spans = v;
                s->cap_spans = cap;
            }
            memmove(s->spans + i + 1, s->spans + i, (s->nspans - i) * sizeof *s->spans);
            s->spans[i] = sp;
            s->nspans++;
        }
        s->chunk_used[c] += piece;
        s->spool_bytes += piece;
        s->free_est -= piece;
        s->res->spooled += piece;
        if (s->spool_bytes > s->res->spool_peak) {
            s->res->spool_peak = s->spool_bytes;
        }
        data += piece;
        off += piece;
    }
}

/* --- files ------------------------------------------------------------------------- */

static xc_err flush(seq *s)
{
    if (s->buffered == 0) {
        return XC_OK;
    }
    xc_err err = s->req->sink->write(s->req->sink->ctx, s->req->buf, s->buffered);
    s->buffered = 0;
    return err;
}

static xc_err begin(seq *s, size_t i)
{
    const xc_xdvdfs_entry *f = &s->listing.files[i];
    note_file(s, f->path);
    if (!make_room(s, rounded(s, f->size))) {
        if (s->err != XC_OK) {
            return s->err;
        }
        s->res->space_needed = rounded(s, f->size) + s->req->reserve;
        s->res->space_free = s->free_est;
        return XC_ERR_NO_SPACE;
    }
    xc_err err = s->req->sink->begin_file(s->req->sink->ctx, f->path, f->size);
    if (err != XC_OK) {
        return err;
    }
    s->free_est -= rounded(s, f->size);
    s->writing = i;
    s->wdone = 0;
    s->buffered = 0;
    s->crc = 0;
    return XC_OK;
}

/* Appends to the open file through the buffer. */
static xc_err put(seq *s, const uint8_t *data, size_t n)
{
    s->crc = xc_crc32(s->crc, data, n);
    s->wdone += n;
    while (n > 0) {
        size_t room = s->req->buf_size - s->buffered;
        size_t k = n < room ? n : room;
        memcpy(s->req->buf + s->buffered, data, k);
        s->buffered += k;
        data += k;
        n -= k;
        if (s->buffered == s->req->buf_size) {
            xc_err err = flush(s);
            if (err != XC_OK) {
                return err;
            }
        }
    }
    return XC_OK;
}

/* Writes straight to the open file (after what's buffered), for bytes already in a buffer. */
static xc_err put_direct(seq *s, const uint8_t *data, size_t n)
{
    xc_err err = flush(s);
    if (err != XC_OK) {
        return err;
    }
    s->crc = xc_crc32(s->crc, data, n);
    s->wdone += n;
    return s->req->sink->write(s->req->sink->ctx, data, n);
}

static xc_err finish(seq *s, bool from_spool)
{
    size_t i = s->writing;
    const xc_xdvdfs_entry *f = &s->listing.files[i];
    xc_err err = flush(s);
    if (err != XC_OK) {
        return err;
    }
    s->writing = NONE;
    err = s->req->sink->end_file(s->req->sink->ctx, true);
    if (err != XC_OK) {
        return err;
    }
    s->state[i] = F_DONE;
    err = pathset_add(&s->done, f->path);
    if (err != XC_OK) {
        return err;
    }
    s->res->bytes_written += f->size;
    if (from_spool) {
        s->res->files_from_spool++;
    } else {
        s->res->files_written++;
    }
    err = s->req->file_done != NULL ? s->req->file_done(s->req->ctx, f, s->crc) : XC_OK;
    if (err == XC_OK && !progress(s, true)) {
        err = XC_ERR_CANCELLED; /* a report after every file, too */
    }
    return err;
}

/* Makes the directories the listing has that don't exist yet (parents come first). */
static xc_err make_dirs(seq *s)
{
    for (size_t i = 0; i < s->listing.dir_count; i++) {
        const char *path = s->listing.dirs[i].path;
        if (pathset_has(&s->made, path)) {
            continue;
        }
        xc_err err = s->req->sink->make_dir(s->req->sink->ctx, path);
        if (err == XC_OK) {
            err = pathset_add(&s->made, path);
        }
        if (err != XC_OK) {
            return err;
        }
    }
    return XC_OK;
}

/* The next direct file at or after listing index `from`, or NONE. */
static size_t next_direct(const seq *s, size_t from)
{
    for (size_t i = from; i < s->listing.file_count; i++) {
        if (s->state[i] == F_DIRECT) {
            return i;
        }
    }
    return NONE;
}

/*
 * Classifies every file of the listing (§3.2): done; empty (written now); direct (it starts
 * at or after `at`, the stream's position); or late.
 */
static xc_err classify(seq *s, uint64_t at)
{
    uint64_t last_end = 0;
    for (size_t i = 0; i < s->listing.file_count; i++) {
        const xc_xdvdfs_entry *f = &s->listing.files[i];
        if (pathset_has(&s->done, f->path)) {
            s->state[i] = F_DONE;
            continue;
        }
        if (s->req->is_done != NULL && s->req->is_done(s->req->ctx, f)) {
            s->state[i] = F_DONE; /* an earlier attempt wrote it */
            xc_err err = pathset_add(&s->done, f->path);
            if (err != XC_OK) {
                return err;
            }
            s->res->files_skipped++;
            continue;
        }
        if (f->size == 0) {
            xc_err err = begin(s, i);
            if (err == XC_OK) {
                err = finish(s, false);
            }
            if (err != XC_OK) {
                return err;
            }
            continue;
        }
        if (f->offset < last_end) {
            return XC_ERR_UNSUPPORTED; /* §3.2: files that share data */
        }
        last_end = f->offset + f->size;
        s->state[i] = f->offset >= at ? F_DIRECT : F_LATE;
    }
    s->dcur = next_direct(s, 0);
    return XC_OK;
}

/* Lists again with every table captured so far, until no more can be filled (§3.1). */
static xc_err relist(seq *s)
{
    for (;;) {
        xc_source src;
        memset(&src, 0, sizeof src);
        src.read_at = read_table;
        src.ctx = s;
        src.size = s->req->image_size;
        xc_xdvdfs_listing l;
        xc_err err = xc_xdvdfs_list_partial(&src, &s->res->vol, s->req->limits, have_table, s, &l);
        if (err != XC_OK) {
            return err;
        }
        bool added_any = false;
        for (size_t i = 0; i < l.dir_count; i++) {
            if (l.dir_pending != NULL && l.dir_pending[i]) {
                bool added = false;
                err = want_table(s, l.dirs[i].offset, l.dirs[i].size, &added);
                if (err != XC_OK) {
                    xc_xdvdfs_listing_free(&l);
                    return err;
                }
                added_any |= added;
            }
        }
        xc_xdvdfs_listing_free(&s->listing);
        s->listing = l;
        file_state *st = realloc(s->state, (l.file_count > 0 ? l.file_count : 1) * sizeof *st);
        if (st == NULL) {
            return XC_ERR_NOMEM;
        }
        s->state = st;
        if (!added_any || !fill_tables_from_spool(s)) {
            break;
        }
    }
    if (s->err != XC_OK) {
        return s->err;
    }
    s->all_known = s->listing.pending_count == 0;
    xc_err err = XC_OK;
    if (s->req->on_listing != NULL) {
        err = s->req->on_listing(s->req->ctx, &s->res->vol, &s->listing, s->all_known);
    }
    if (err == XC_OK) {
        err = make_dirs(s);
    }
    if (err == XC_OK) {
        err = classify(s, s->pos);
    }
    if (err == XC_OK && s->all_known) {
        s->res->file_count = s->listing.file_count;
    }
    return err;
}

/* --- volume detection (§2) ---------------------------------------------------------- */

static xc_err vd_read(void *ctx, uint64_t offset, void *buf, size_t len)
{
    const seq *s = ctx;
    uint64_t at = candidates[s->cand] + VD_OFFSET;
    memset(buf, 0, len);
    if (offset < at + SECTOR && offset + len > at) {
        uint64_t a = offset > at ? offset : at;
        uint64_t b = offset + len < at + SECTOR ? offset + len : at + SECTOR;
        memcpy((uint8_t *)buf + (a - offset), s->vd + (a - at), (size_t)(b - a));
    }
    return XC_OK;
}

/* Decides on candidate s->cand once its descriptor has passed. */
static xc_err decide(seq *s)
{
    xc_source src;
    memset(&src, 0, sizeof src);
    src.read_at = vd_read;
    src.ctx = s;
    src.size = s->req->image_size;
    xc_xdvdfs_volume vol;
    xc_err err = xc_xdvdfs_open(&src, &vol);
    if (err == XC_ERR_NOT_XDVDFS || (err == XC_OK && vol.partition_offset != candidates[s->cand])) {
        s->cand++;
        memset(s->vd, 0, sizeof s->vd);
        return XC_OK;
    }
    if (err != XC_OK) {
        return err;
    }
    s->res->vol = vol;
    s->vol_known = true;
    bool added = false;
    uint64_t root = vol.partition_offset + (uint64_t)vol.root_sector * SECTOR;
    err = want_table(s, root, vol.root_size, &added);
    if (err != XC_OK) {
        return err;
    }
    /* §2.3: the partition's bytes before the descriptor are unidentified like any other */
    spool_put(s, vol.partition_offset, s->pre, VD_OFFSET);
    if (s->err != XC_OK) {
        return s->err;
    }
    (void)capture(s, vol.partition_offset, s->pre, VD_OFFSET);
    free(s->pre);
    s->pre = NULL;
    if (vol.root_size == 0) {
        return relist(s); /* no files at all */
    }
    if (fill_tables_from_spool(s)) {
        return relist(s);
    }
    return s->err;
}

/* Consumes stream bytes before the layout is known; returns how many. */
static uint64_t detect(seq *s, uint64_t off, const uint8_t *data, uint64_t n)
{
    while (s->cand < NCANDIDATES && candidates[s->cand] + VD_OFFSET + SECTOR > s->req->image_size) {
        s->cand++; /* §2.1: its descriptor would end past the image */
    }
    if (s->cand >= NCANDIDATES) {
        s->err = XC_ERR_NOT_XDVDFS;
        return n;
    }
    uint64_t part = candidates[s->cand];
    uint64_t vd = part + VD_OFFSET;
    uint64_t end = vd + SECTOR;
    uint64_t take = n < end - off ? n : end - off; /* up to the descriptor's end */
    if (off < vd && off + take > part) {
        uint64_t a = off > part ? off : part;
        uint64_t b = off + take < vd ? off + take : vd;
        memcpy(s->pre + (a - part), data + (a - off), (size_t)(b - a));
    }
    if (off + take > vd) {
        uint64_t a = off > vd ? off : vd;
        memcpy(s->vd + (a - vd), data + (a - off), (size_t)(off + take - a));
    }
    if (off + take == end) {
        s->pos = end;
        s->err = decide(s);
    }
    return take;
}

/* --- the stream ------------------------------------------------------------------- */

/*
 * In a later pass, bytes of files already written aren't spooled again. True if `off` is
 * inside one (*k is then cut to its end); otherwise *k is cut to the next one's start.
 */
static bool skip_written(const seq *s, uint64_t off, uint64_t *k)
{
    const xc_xdvdfs_listing *l = &s->listing;
    size_t lo = 0;
    size_t hi = l->file_count;
    while (lo < hi) { /* the first file ending after `off` */
        size_t mid = lo + (hi - lo) / 2;
        if (l->files[mid].offset + l->files[mid].size <= off) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    for (size_t i = lo; i < l->file_count; i++) {
        const xc_xdvdfs_entry *f = &l->files[i];
        if (f->offset >= off + *k) {
            break;
        }
        if (s->state[i] != F_DONE || f->size == 0) {
            continue;
        }
        if (f->offset <= off) {
            uint64_t end = f->offset + f->size - off;
            *k = end < *k ? end : *k;
            return true;
        }
        *k = f->offset - off;
        break;
    }
    return false;
}

/* One piece of the stream at the current position (§3). */
static void consume(seq *s, const uint8_t *data, uint64_t n)
{
    while (n > 0 && s->err == XC_OK && !s->stop) {
        uint64_t off = s->pos;
        uint64_t k = 0;
        if (!s->vol_known) {
            k = detect(s, off, data, n);
            s->pos = off + k;
        } else if (s->writing != NONE) {
            const xc_xdvdfs_entry *f = &s->listing.files[s->writing];
            k = f->offset + f->size - off;
            k = k < n ? k : n;
            s->err = put(s, data, (size_t)k);
            s->pos = off + k;
            if (s->err == XC_OK && s->wdone == f->size) {
                s->err = finish(s, false);
                s->dcur = next_direct(s, s->dcur + 1);
            }
        } else {
            uint64_t file_at = s->dcur != NONE ? s->listing.files[s->dcur].offset : UINT64_MAX;
            bool in_table = false;
            uint64_t table_at = next_wanted(s, off, &in_table);
            if (file_at <= off) {
                s->err = begin(s, s->dcur);
                continue;
            }
            uint64_t bound = file_at < table_at ? file_at : table_at;
            if (in_table) {
                /* up to the end of the overlapping tables, or the next file */
                bound = file_at;
                for (size_t i = 0; i < s->ntables; i++) {
                    const table *t = &s->tables[i];
                    if (t->got != NULL && t->off <= off && t->off + t->len > off &&
                        t->off + t->len < bound) {
                        bound = t->off + t->len;
                    }
                }
            }
            k = bound - off < n ? bound - off : n;
            if (in_table) {
                bool completed = capture(s, off, data, k);
                s->pos = off + k;
                if (completed) {
                    s->err = relist(s);
                }
            } else if (skip_written(s, off, &k)) {
                s->pos = off + k; /* bytes of a file already written (a later pass) */
            } else {
                if (!s->all_known && off + k > s->res->vol.partition_offset) {
                    uint64_t a =
                        off > s->res->vol.partition_offset ? off : s->res->vol.partition_offset;
                    spool_put(s, a, data + (a - off), off + k - a);
                }
                s->pos = off + k;
            }
        }
        data += k;
        n -= k;
        if (s->err == XC_OK && !progress(s, false)) {
            return;
        }
        if (s->vol_known && s->all_known && s->writing == NONE && s->dcur == NONE &&
            !(s->pass == 1 && s->req->full_first_pass)) {
            s->stop = true; /* §3.3: nothing ahead is needed */
        }
    }
}

static xc_err on_stream(void *ctx, uint64_t offset, const uint8_t *data, size_t len)
{
    seq *s = ctx;
    s->res->streamed += len;
    if (s->pass > 1) {
        s->res->reread += len;
    }
    if (offset + len <= s->pos) {
        return XC_OK; /* before where this pass needs to start */
    }
    if (offset > s->pos) {
        return XC_ERR_INVALID_ARG; /* the stream must start at or before `from` */
    }
    uint64_t skip = s->pos - offset;
    consume(s, data + skip, len - skip);
    if (s->err != XC_OK) {
        return s->err;
    }
    return s->stop ? XC_ERR_CANCELLED : XC_OK;
}

/* --- passes ----------------------------------------------------------------------- */

/* §3.3: copies every late file whose bytes are all in the spool, in offset order. */
static xc_err resolve(seq *s)
{
    for (size_t i = 0; i < s->listing.file_count; i++) {
        const xc_xdvdfs_entry *f = &s->listing.files[i];
        if (s->state[i] != F_LATE || !spooled(s, f->offset, f->size)) {
            continue;
        }
        if (s->all_known) {
            /* chunks entirely before this file hold nothing needed any more */
            for (size_t j = 0; j < s->nchunks; j++) {
                uint64_t chunk_end = ((uint64_t)j + 1) * chunk_bytes(s);
                if (s->chunk_used[j] > 0 && chunk_end <= f->offset) {
                    xc_err err = drop_chunk(s, (uint32_t)j);
                    if (err != XC_OK) {
                        return err;
                    }
                }
            }
        }
        /* making room for it must not drop the chunks that hold it */
        s->copying = true;
        s->keep_lo = (uint32_t)(f->offset / chunk_bytes(s));
        s->keep_hi = (uint32_t)((f->offset + f->size - 1) / chunk_bytes(s));
        xc_err err = begin(s, i);
        s->copying = false;
        for (uint64_t p = f->offset; err == XC_OK && p < f->offset + f->size;) {
            size_t n = (size_t)(f->offset + f->size - p < s->req->buf_size ? f->offset + f->size - p
                                                                           : s->req->buf_size);
            err = spool_read(s, p, s->req->buf, n);
            if (err == XC_OK) {
                err = put_direct(s, s->req->buf, n);
            }
            p += n;
        }
        if (err == XC_OK) {
            err = finish(s, true);
        } else if (s->writing != NONE) {
            s->writing = NONE;
            (void)s->req->sink->end_file(s->req->sink->ctx, false);
        }
        if (err != XC_OK) {
            return err;
        }
    }
    if (s->all_known) {
        for (size_t j = 0; j < s->nchunks; j++) {
            bool needed = false;
            uint64_t a = (uint64_t)j * chunk_bytes(s);
            uint64_t b = a + chunk_bytes(s);
            for (size_t i = 0; i < s->listing.file_count && !needed; i++) {
                const xc_xdvdfs_entry *f = &s->listing.files[i];
                needed = s->state[i] == F_LATE && f->offset < b && f->offset + f->size > a;
            }
            if (!needed) {
                xc_err err = drop_chunk(s, (uint32_t)j);
                if (err != XC_OK) {
                    return err;
                }
            }
        }
    }
    return XC_OK;
}

/* §3.4: where the next pass must start, or UINT64_MAX if nothing is missing. */
static uint64_t next_from(const seq *s)
{
    uint64_t from = UINT64_MAX;
    for (size_t i = 0; i < s->ntables; i++) {
        const table *t = &s->tables[i];
        if (t->got != NULL) {
            for (size_t k = 0; k < t->len; k++) {
                if (!t->got[k]) {
                    from = t->off + k < from ? t->off + k : from;
                    break;
                }
            }
        }
    }
    for (size_t i = 0; i < s->listing.file_count; i++) {
        if (s->state[i] == F_LATE && s->listing.files[i].offset < from) {
            from = s->listing.files[i].offset;
        }
    }
    return from;
}

static xc_err run_pass(seq *s, uint64_t from)
{
    s->pos = from;
    s->stop = false;
    s->next_progress = from;
    if (s->vol_known) {
        xc_err err = classify(s, from);
        if (err != XC_OK) {
            return err;
        }
    }
    xc_err err = s->req->stream(s->req->stream_ctx, from, on_stream, s);
    if (s->err != XC_OK) {
        err = s->err;
    } else if (err == XC_ERR_CANCELLED && s->stop) {
        err = XC_OK;
    }
    if (err != XC_OK) {
        if (s->writing != NONE) {
            s->writing = NONE;
            (void)s->req->sink->end_file(s->req->sink->ctx, false);
        }
        return err;
    }
    if (!s->vol_known) {
        return XC_ERR_NOT_XDVDFS;
    }
    if (s->writing != NONE) {
        return XC_ERR_CORRUPT; /* the stream ended inside a file */
    }
    return resolve(s);
}

static xc_err init(seq *s, const xc_seq_req *req, xc_seq_result *res)
{
    memset(s, 0, sizeof *s);
    s->req = req;
    s->res = res;
    s->dcur = NONE;
    s->writing = NONE;
    uint64_t cb = chunk_bytes(s);
    uint64_t n = req->image_size / cb + 1;
    if (n > MAX_CHUNKS) {
        return XC_ERR_INVALID_ARG;
    }
    s->nchunks = (size_t)n;
    s->chunk_used = calloc(s->nchunks, sizeof *s->chunk_used);
    s->pre = calloc(VD_OFFSET, 1);
    if (s->chunk_used == NULL || s->pre == NULL) {
        return XC_ERR_NOMEM;
    }
    return req->spool->free_space(req->spool->ctx, &s->free_est);
}

static void cleanup(seq *s)
{
    for (size_t i = 0; i < s->ntables; i++) {
        free(s->tables[i].data);
        free(s->tables[i].got);
    }
    free(s->tables);
    xc_xdvdfs_listing_free(&s->listing);
    free(s->state);
    pathset_free(&s->done);
    pathset_free(&s->made);
    free(s->spans);
    free(s->chunk_used);
    free(s->pre);
}

xc_err xc_seq_extract(const xc_seq_req *req, xc_seq_result *res)
{
    xc_seq_result local;
    if (res == NULL) {
        res = &local;
    }
    memset(res, 0, sizeof *res);
    if (req == NULL || req->stream == NULL || req->sink == NULL || req->spool == NULL ||
        req->buf == NULL || req->buf_size < 65536) {
        return XC_ERR_INVALID_ARG;
    }
    seq s;
    xc_err err = init(&s, req, res);
    unsigned max = req->max_passes > 0 ? req->max_passes : XC_SEQ_DEFAULT_MAX_PASSES;
    uint64_t from = 0;
    while (err == XC_OK) {
        s.pass++;
        res->passes = s.pass;
        err = run_pass(&s, from);
        if (err != XC_OK) {
            break;
        }
        from = next_from(&s);
        if (from == UINT64_MAX) {
            break; /* everything is written */
        }
        if (s.pass == max) {
            err = XC_ERR_LIMIT;
        }
    }
    if (err == XC_OK) {
        /* the spool is empty by now; drop anything left anyway */
        for (size_t j = 0; j < s.nchunks && err == XC_OK; j++) {
            err = drop_chunk(&s, (uint32_t)j);
        }
        (void)progress(&s, true);
        if (req->listing_out != NULL) {
            *req->listing_out = s.listing;
            memset(&s.listing, 0, sizeof s.listing);
        }
    }
    cleanup(&s);
    return err;
}
