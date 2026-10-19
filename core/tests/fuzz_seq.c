/*
 * libFuzzer target for sequential extraction (docs/formats/stream-install.md). The input is
 * an image streamed front to back; its last byte also sets the disk size (see below), so
 * the fuzzer explores spools that drop bytes and installs that need several passes.
 *
 * Checked; a violation aborts: the sink sees at most one open file, writes stay inside each
 * file's size, a file reported done got exactly the image's bytes at its offset (and that
 * CRC-32), the spool never holds more than the disk, and after a successful
 * install the spool is empty. Seeds are fixture images copied at build time. Built like
 * fuzz_http.c.
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
    uint64_t disk;
    uint64_t used;
    uint8_t *chunk[MAX_CHUNK_IDS];
    size_t chunk_len[MAX_CHUNK_IDS];
} world;

static world w;

static xc_err stream(void *ctx, uint64_t from, xc_source_data_fn fn, void *fn_ctx)
{
    (void)ctx;
    uint64_t off = from - from % 4096U;
    while (off < w.img_size) {
        size_t n = (size_t)(w.img_size - off < 3001 ? w.img_size - off : 3001);
        xc_err err = fn(fn_ctx, off, w.img + off, n);
        if (err != XC_OK) {
            return err;
        }
        off += n;
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
    (void)path;
    check(!w.open);
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
    w.used -= w.chunk_len[c];
    free(w.chunk[c]);
    w.chunk[c] = NULL;
    w.chunk_len[c] = 0;
    return XC_OK;
}

static xc_err s_free(void *ctx, uint64_t *bytes)
{
    (void)ctx;
    *bytes = w.disk - w.used;
    return XC_OK;
}

static xc_err file_done(void *ctx, const xc_xdvdfs_entry *f, uint32_t crc)
{
    (void)ctx;
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

    static const xc_sink sink = {make_dir, begin, write_fn, end, NULL, NULL, NULL, NULL};
    static const xc_seq_spool spool = {s_append, s_read, s_drop, s_free, NULL};
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
    xc_seq_result r;
    xc_err err = xc_seq_extract(&q, &r);
    check(!w.open);
    if (err == XC_OK) {
        for (size_t c = 0; c < MAX_CHUNK_IDS; c++) {
            check(w.chunk_len[c] == 0);
        }
    }
    for (size_t c = 0; c < MAX_CHUNK_IDS; c++) {
        free(w.chunk[c]);
    }
    free(w.file_data);
    return 0;
}
