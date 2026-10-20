/*
 * libFuzzer target for sequential extraction (docs/formats/stream-install.md). The input is
 * an image streamed front to back; its last byte also sets the disk size (see below), so
 * the fuzzer explores spools that drop bytes and installs that need several passes. The
 * byte before it can cut the power at some point of some pass, with a state saved every
 * 4 KiB (§7), and resume from the last one; or resume from a state with one byte forged
 * (and its CRC-32 fixed), to exercise the state's checks.
 *
 * Checked; a violation aborts: the sink sees at most one open file, writes stay inside each
 * file's size, a file reported done got exactly the image's bytes at its offset (and that
 * CRC-32), the spool never holds more than the disk, and after a successful
 * install the spool is empty. With a forged state only the first two are checked. Seeds are
 * fixture images copied at build time. Built like fuzz_http.c.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xcat/crc32.h"
#include "xcat/seq.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#define MAX_CHUNK_IDS 64

static void check_at(int ok, int line)
{
    if (!ok) {
        (void)fprintf(stderr, "fuzz_seq: check at line %d failed\n", line);
        abort();
    }
}
#define check(ok) check_at((ok), __LINE__)

typedef struct world {
    const uint8_t *img;
    uint64_t img_size;
    bool open;
    uint32_t file_size;
    uint32_t file_len;
    uint32_t file_crc;
    uint8_t *file_data; /* what the open file got, to compare byte by byte */
    char file_path[1024];
    uint64_t disk;
    uint64_t used;
    uint8_t *chunk[MAX_CHUNK_IDS];
    size_t chunk_len[MAX_CHUNK_IDS];
    bool dropped_since[MAX_CHUNK_IDS]; /* since the last state */
    /* §7 */
    bool save;
    unsigned streams;
    unsigned cut_stream; /* 0 = none */
    uint64_t cut_at;
    uint8_t *state;
    size_t state_len;
    bool forged; /* only memory safety and the sink's rules are checked */
    char **done; /* paths written so far */
    uint32_t *done_size;
    size_t ndone, cap_done;
} world;

static world w;

static void take_state(void *fn_ctx, uint64_t at)
{
    uint8_t *b = NULL;
    size_t len = 0;
    check(xc_seq_save(fn_ctx, at, &b, &len) == XC_OK);
    if (b != NULL) {
        uint64_t back = 0;
        check(xc_seq_state_offset(b, len, w.img_size, 65536, &back) == XC_OK && back == at);
        free(w.state);
        w.state = b;
        w.state_len = len;
        memset(w.dropped_since, 0, sizeof w.dropped_since);
    }
}

static xc_err stream(void *ctx, uint64_t from, xc_source_data_fn fn, void *fn_ctx)
{
    (void)ctx;
    w.streams++;
    uint64_t off = from - from % 4096U;
    while (off < w.img_size) {
        size_t n = (size_t)(w.img_size - off < 3001 ? w.img_size - off : 3001);
        if (w.save && 4096U - off % 4096U < n) {
            n = (size_t)(4096U - off % 4096U);
        }
        bool cut = w.cut_stream == w.streams && off + n >= w.cut_at;
        if (cut) {
            n = (size_t)(w.cut_at > off ? w.cut_at - off : 0);
        }
        if (n > 0) {
            xc_err err = fn(fn_ctx, off, w.img + off, n);
            if (err != XC_OK) {
                return err;
            }
        }
        if (cut) {
            return XC_ERR_IO;
        }
        off += n;
        if (w.save && off % 4096U == 0 && off < w.img_size) {
            take_state(fn_ctx, off);
        }
    }
    return XC_OK;
}

static xc_err make_dir(void *ctx, const char *path)
{
    (void)ctx;
    check(path != NULL && path[0] != '\0');
    return XC_OK;
}

static xc_err begin(void *ctx, const char *path, uint32_t size)
{
    (void)ctx;
    check(!w.open);
    (void)snprintf(w.file_path, sizeof w.file_path, "%s", path);
    w.open = true;
    w.file_size = size;
    w.file_len = 0;
    w.file_crc = 0;
    free(w.file_data);
    w.file_data = malloc(size > 0 ? size : 1U);
    check(w.file_data != NULL);
    w.used += size;
    return XC_OK;
}

static xc_err write_fn(void *ctx, const void *buf, size_t len)
{
    (void)ctx;
    check(w.open && len <= w.file_size - w.file_len);
    w.file_crc = xc_crc32(w.file_crc, buf, len);
    memcpy(w.file_data + w.file_len, buf, len);
    w.file_len += (uint32_t)len;
    return XC_OK;
}

static xc_err end(void *ctx, bool complete)
{
    (void)ctx;
    check(w.open && (!complete || w.file_len == w.file_size));
    w.open = false;
    if (!complete) {
        w.file_path[0] = '\0'; /* removed */
    }
    return XC_OK;
}

static xc_err sync_fn(void *ctx)
{
    (void)ctx;
    check(w.open);
    return XC_OK;
}

/* Only the last file can be continued: it's the one the cut left open. */
static xc_err resume_fn(void *ctx, const char *path, uint32_t size, uint32_t keep)
{
    (void)ctx;
    check(!w.open);
    if (strcmp(path, w.file_path) != 0 || size != w.file_size || keep > w.file_len) {
        return XC_ERR_IO;
    }
    w.open = true;
    w.file_len = keep;
    w.file_crc = xc_crc32(0, w.file_data, keep);
    return XC_OK;
}

static xc_err suspend_fn(void *ctx)
{
    (void)ctx;
    check(w.open);
    w.open = false;
    return XC_OK;
}

static xc_err s_append(void *ctx, uint32_t c, const uint8_t *data, size_t len)
{
    (void)ctx;
    check(c < MAX_CHUNK_IDS && w.used + len <= w.disk);
    uint8_t *p = realloc(w.chunk[c], w.chunk_len[c] + len);
    if (p == NULL) {
        return XC_ERR_NOMEM;
    }
    memcpy(p + w.chunk_len[c], data, len);
    w.chunk[c] = p;
    w.chunk_len[c] += len;
    w.used += len;
    return XC_OK;
}

static xc_err s_read(void *ctx, uint32_t c, uint64_t pos, uint8_t *buf, size_t len)
{
    (void)ctx;
    check(c < MAX_CHUNK_IDS && pos <= w.chunk_len[c] && len <= w.chunk_len[c] - pos);
    memcpy(buf, w.chunk[c] + pos, len);
    return XC_OK;
}

static xc_err s_drop(void *ctx, uint32_t c)
{
    (void)ctx;
    check(c < MAX_CHUNK_IDS);
    w.dropped_since[c] = w.state != NULL;
    w.used -= w.chunk_len[c];
    free(w.chunk[c]);
    w.chunk[c] = NULL;
    w.chunk_len[c] = 0;
    return XC_OK;
}

static xc_err s_free(void *ctx, uint64_t *bytes)
{
    (void)ctx;
    *bytes = w.used < w.disk ? w.disk - w.used : 0;
    return XC_OK;
}

static xc_err s_size(void *ctx, uint32_t c, uint64_t *bytes)
{
    (void)ctx;
    *bytes = c < MAX_CHUNK_IDS ? w.chunk_len[c] : 0;
    return XC_OK;
}

static xc_err s_sync(void *ctx)
{
    (void)ctx;
    return XC_OK;
}

static bool is_done(void *ctx, const xc_xdvdfs_entry *f)
{
    (void)ctx;
    for (size_t i = 0; i < w.ndone; i++) {
        if (w.done_size[i] == f->size && strcmp(w.done[i], f->path) == 0) {
            return true;
        }
    }
    return false;
}

static xc_err file_done(void *ctx, const xc_xdvdfs_entry *f, uint32_t crc)
{
    (void)ctx;
    if (w.ndone == w.cap_done) {
        size_t cap = w.cap_done > 0 ? w.cap_done * 2 : 64;
        char **d = (char **)realloc((void *)w.done, cap * sizeof *d);
        uint32_t *s = d != NULL ? realloc(w.done_size, cap * sizeof *s) : NULL;
        if (d != NULL) {
            w.done = d;
        }
        if (s == NULL) {
            return XC_ERR_NOMEM;
        }
        w.done_size = s;
        w.cap_done = cap;
    }
    size_t plen = strlen(f->path) + 1;
    w.done[w.ndone] = malloc(plen);
    if (w.done[w.ndone] == NULL) {
        return XC_ERR_NOMEM;
    }
    memcpy(w.done[w.ndone], f->path, plen);
    w.done_size[w.ndone++] = f->size;
    if (w.forged) {
        return XC_OK;
    }
    check(crc == w.file_crc && w.file_len == f->size);
    if (f->size == 0) {
        return XC_OK; /* its sector isn't checked, so it may point anywhere (xdvdfs.md §5) */
    }
    check(f->offset <= w.img_size && f->size <= w.img_size - f->offset);
    if (memcmp(w.file_data, w.img + f->offset, f->size) != 0) {
        size_t k = 0;
        while (k < f->size && w.file_data[k] == w.img[f->offset + k]) {
            k++;
        }
        (void)fprintf(stderr, "fuzz_seq: %s (offset %llu, size %u) differs from byte %zu\n",
                      f->path, (unsigned long long)f->offset, f->size, k);
        check(0);
    }
    return XC_OK;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 2 || size > (size_t)4 * 1024 * 1024) {
        return 0;
    }
    memset(&w, 0, sizeof w);
    w.img = data;
    w.img_size = size;
    /* the last byte: 0 = a disk as big as the image, else image * (byte / 64) / 4 */
    uint8_t cfg = data[size - 1];
    w.disk = cfg == 0 ? size : (uint64_t)size * (cfg / 64U + 1U) / 4U;
    /* the byte before: 0 = no cut; else a cut in pass (cut & 3) + 1, at size * cut / 256,
       and with 0x80 forged, a byte of the state changed (and its CRC fixed) */
    uint8_t cut = data[size - 2];
    w.save = cut != 0;
    w.cut_stream = cut != 0 ? (cut & 3U) + 1U : 0;
    w.cut_at = (uint64_t)size * (cut & 0x7FU) / 128U;

    static const xc_sink sink = {make_dir, begin,   write_fn,  end,
                                 NULL,     sync_fn, resume_fn, suspend_fn};
    static const xc_seq_spool spool = {s_append, s_read, s_drop, s_free, NULL, s_size, s_sync};
    static uint8_t buf[65536];
    xc_xdvdfs_limits lim;
    xc_xdvdfs_default_limits(&lim);
    lim.max_entries = 4096;
    xc_seq_req q;
    memset(&q, 0, sizeof q);
    q.image_size = size;
    q.stream = stream;
    q.sink = &sink;
    q.spool = &spool;
    q.chunk_bytes = 65536;
    q.limits = &lim;
    q.buf = buf;
    q.buf_size = sizeof buf;
    q.max_passes = 8;
    q.file_done = file_done;
    q.is_done = is_done;
    xc_seq_result r;
    xc_err err = xc_seq_extract(&q, &r);
    check(!w.open);
    if (err == XC_ERR_IO && w.cut_stream > 0) {
        /* the power went: chunks dropped since the state go (install.c's `drop` lines) */
        for (uint32_t c = 0; c < MAX_CHUNK_IDS; c++) {
            if (w.dropped_since[c]) {
                (void)s_drop(NULL, c);
            }
        }
        w.cut_stream = 0;
        w.save = false;
        w.streams = 0;
        if ((cut & 0x80U) != 0 && w.state != NULL && w.state_len > 4) {
            size_t at = (size_t)((data[0] * 7919U + data[1]) % (w.state_len - 4));
            w.state[at] ^= (uint8_t)(data[size / 2] | 1U);
            uint32_t crc = xc_crc32(0, w.state, w.state_len - 4);
            for (unsigned i = 0; i < 4; i++) {
                w.state[w.state_len - 4 + i] = (uint8_t)(crc >> (8U * i));
            }
            w.forged = true;
        }
        q.resume = w.state;
        q.resume_len = w.state_len;
        err = xc_seq_extract(&q, &r);
        check(!w.open);
    }
    if (err == XC_OK) {
        for (size_t c = 0; c < MAX_CHUNK_IDS; c++) {
            check(w.chunk_len[c] == 0);
        }
    }
    for (size_t c = 0; c < MAX_CHUNK_IDS; c++) {
        free(w.chunk[c]);
    }
    for (size_t i = 0; i < w.ndone; i++) {
        free(w.done[i]);
    }
    free((void *)w.done);
    free(w.done_size);
    free(w.state);
    free(w.file_data);
    return 0;
}
