/*
 * libFuzzer target for the XDVDFS reader (docs/formats/xdvdfs.md).
 *
 * Input: [u32 root sector delta][u32 root table size][bytes...], little-endian.
 * The harness builds a virtual image around it, so the fuzzer spends its time
 * on the directory-tree walk instead of on reaching a volume descriptor:
 *
 *   [0, sector 32)          zeros
 *   sector 32               a valid volume descriptor -> root at `base`
 *   [sector 33, base)       zeros
 *   [base, base + n)        the input bytes after the 8-byte header
 *   then 64 MiB of zeros    so file extents past the input stay in range
 *
 * base = sector 33 + (delta % 4096). Seeds come from tools/fixtures/fuzz_seeds.py.
 * The raw input is also tried as a whole image. Successful listings are checked
 * against the reader's documented guarantees; a violation aborts.
 *
 * Built two ways: with -fsanitize=fuzzer (XCAT_FUZZ, clang only), and as a
 * plain replay program (fuzz_xdvdfs_replay) that every compiler builds and CTest
 * runs over the seeds.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "xcat/xdvdfs.h"

#define SECTOR 2048U
#define VD_SECTOR 32U
#define FIRST_ROOT_SECTOR 33U
#define MAX_DELTA 4096U
#define ZERO_TAIL (64ULL * 1024ULL * 1024ULL)

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static const uint8_t magic[20] = {'M', 'I', 'C', 'R', 'O', 'S', 'O', 'F', 'T', '*',
                                  'X', 'B', 'O', 'X', '*', 'M', 'E', 'D', 'I', 'A'};

typedef struct vimg {
    const uint8_t *data;
    size_t len;
    uint64_t base; /* byte offset of the input data */
    uint8_t vd[SECTOR];
    uint64_t size;
} vimg;

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) | ((uint32_t)p[2] << 16U) |
           ((uint32_t)p[3] << 24U);
}

static void wr32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; i++) {
        p[i] = (uint8_t)((v >> (8U * i)) & 0xFFU);
    }
}

static uint64_t min_u64(uint64_t a, uint64_t b)
{
    return a < b ? a : b;
}

static xc_err vimg_read(void *ctx, uint64_t offset, void *buf, size_t len)
{
    const vimg *v = ctx;
    uint8_t *out = buf;
    if (offset > v->size || (uint64_t)len > v->size - offset) {
        return XC_ERR_RANGE;
    }
    const uint64_t vd_start = (uint64_t)VD_SECTOR * SECTOR;
    const uint64_t vd_end = vd_start + SECTOR;
    const uint64_t data_end = v->base + v->len;
    size_t done = 0;
    while (done < len) {
        uint64_t pos = offset + done;
        uint64_t left = len - done;
        size_t n = 0;
        if (pos >= vd_start && pos < vd_end) {
            n = (size_t)min_u64(left, vd_end - pos);
            memcpy(out + done, v->vd + (pos - vd_start), n);
        } else if (pos >= v->base && pos < data_end) {
            n = (size_t)min_u64(left, data_end - pos);
            memcpy(out + done, v->data + (pos - v->base), n);
        } else {
            uint64_t next = pos < vd_start ? vd_start : pos < v->base ? v->base : v->size;
            n = (size_t)min_u64(left, next - pos);
            memset(out + done, 0, n);
        }
        done += n;
    }
    return XC_OK;
}

/* The guarantees in docs/formats/xdvdfs.md §5 and §7; abort if any is broken. */
static void check_listing(const xc_source *src, const xc_xdvdfs_listing *l)
{
    uint64_t total = 0;
    for (size_t i = 0; i < l->file_count; i++) {
        const xc_xdvdfs_entry *f = &l->files[i];
        if (f->path == NULL || f->path[0] == '\0' || (f->attributes & 0x10U) != 0) {
            abort();
        }
        if (f->size > 0 && (f->offset > src->size || f->size > src->size - f->offset)) {
            abort();
        }
        if (i > 0 && l->files[i - 1].offset > f->offset) {
            abort();
        }
        total += f->size;
    }
    for (size_t i = 0; i < l->dir_count; i++) {
        if (l->dirs[i].path == NULL || (l->dirs[i].attributes & 0x10U) == 0) {
            abort();
        }
    }
    if (total != l->total_file_bytes) {
        abort();
    }
}

static void run(const xc_source *src)
{
    xc_xdvdfs_volume vol;
    if (xc_xdvdfs_open(src, &vol) != XC_OK) {
        return;
    }
    xc_xdvdfs_listing l;
    if (xc_xdvdfs_list(src, &vol, NULL, &l) == XC_OK) {
        check_listing(src, &l);
    }
    xc_xdvdfs_listing_free(&l);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* 1. The raw input as a whole image (mostly exercises layout probing). */
    xc_mem_source mem;
    xc_source raw;
    xc_source_memory(&raw, &mem, data, size);
    run(&raw);

    /* 2. The input as a root table inside a virtual image. */
    if (size < 8) {
        return 0;
    }
    vimg v;
    uint32_t root_sector = FIRST_ROOT_SECTOR + rd32(data) % MAX_DELTA;
    v.data = data + 8;
    v.len = size - 8;
    v.base = (uint64_t)root_sector * SECTOR;
    v.size = v.base + v.len + ZERO_TAIL;
    memset(v.vd, 0, sizeof v.vd);
    memcpy(v.vd, magic, sizeof magic);
    wr32(v.vd + 0x14, root_sector);
    wr32(v.vd + 0x18, rd32(data + 4));
    memcpy(v.vd + 0x7EC, magic, sizeof magic);

    xc_source src = {vimg_read, &v, v.size};
    run(&src);
    return 0;
}
