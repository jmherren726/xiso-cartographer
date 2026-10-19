/*
 * libFuzzer target for reading Deflate members with checkpoints (docs/formats/zip.md
 * §9–§10). The input is raw Deflate data (RFC 1951) used as a member's compressed bytes.
 *
 * 1. Read it as a member of up to 256 KiB with no CRC: it must fail (the size and CRC are
 *    unknown) without overrunning, and the end position gives the bytes it decoded.
 * 2. Read it again with that size and CRC-32, and checkpoints every 32 KiB. If the data is a
 *    complete stream, this must succeed with the same CRC-32; then resuming from the first,
 *    middle and last checkpoint must also succeed and end at the same CRC-32.
 * 3. A copy of the first checkpoint with one byte changed (at a place the input picks) is
 *    always refused: XC_ERR_CORRUPT, or XC_ERR_UNSUPPORTED for the magic and build tag.
 *
 * Seeds are in core/tests/fuzz-seeds/zip-inflate/ (tools/fixtures/zip_seeds.py). Built like
 * fuzz_http.c.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "xcat/crc32.h"
#include "xcat/zip.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#define MAX_OUT ((size_t)256 * 1024)
#define MAX_CKS (MAX_OUT / XC_ZIP_DICT_SIZE)

static void check(int ok)
{
    if (!ok) {
        abort();
    }
}

typedef struct seen {
    uint64_t next;
    uint8_t *cks[MAX_CKS];
    size_t ck_len[MAX_CKS];
    xc_zip_position pos[MAX_CKS];
    size_t count;
} seen;

static xc_err on_data(void *ctx, uint64_t offset, const uint8_t *data, size_t len)
{
    seen *s = ctx;
    (void)data;
    check(offset == s->next && len > 0 && offset + len <= MAX_OUT);
    s->next += len;
    return XC_OK;
}

static xc_err on_checkpoint(void *ctx, const xc_zip_position *pos, const uint8_t *blob, size_t len)
{
    seen *s = ctx;
    check(s->count < MAX_CKS && pos->offset % XC_ZIP_DICT_SIZE == 0 && pos->offset == s->next);
    s->cks[s->count] = malloc(len);
    check(s->cks[s->count] != NULL);
    memcpy(s->cks[s->count], blob, len);
    s->ck_len[s->count] = len;
    s->pos[s->count] = *pos;
    s->count++;
    return XC_OK;
}

static xc_err read_once(const xc_source *src, const xc_zip_member *m, seen *s,
                        const uint8_t *resume, size_t resume_len, uint64_t from, bool checkpoints,
                        xc_zip_position *end)
{
    xc_zip_read q;
    memset(&q, 0, sizeof q);
    q.src = src;
    q.member = m;
    q.on_data = on_data;
    q.data_ctx = s;
    q.resume = resume;
    q.resume_len = resume_len;
    q.read_size = 777;
    if (checkpoints) {
        q.checkpoint_every = XC_ZIP_DICT_SIZE;
        q.on_checkpoint = on_checkpoint;
        q.checkpoint_ctx = s;
    }
    s->next = from;
    return xc_zip_read_member(&q, end);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    xc_mem_source mem;
    xc_source src;
    xc_source_memory(&src, &mem, data, size);
    xc_zip_member m = {0, XC_ZIP_DEFLATE, 0, size, MAX_OUT, 0};

    /* 1 */
    seen s1;
    memset(&s1, 0, sizeof s1);
    xc_zip_position end1;
    xc_err err = read_once(&src, &m, &s1, NULL, 0, 0, false, &end1);
    check(err == XC_ERR_CORRUPT || err == XC_ERR_ZIP_CRC || err == XC_OK);
    check(end1.offset == s1.next && end1.compressed <= size);

    /* 2 */
    m.size = end1.offset;
    m.crc32 = end1.crc32;
    seen s2;
    memset(&s2, 0, sizeof s2);
    xc_zip_position end2;
    err = read_once(&src, &m, &s2, NULL, 0, 0, true, &end2);
    if (err == XC_OK) {
        check(end2.offset == m.size && end2.crc32 == m.crc32 && end2.compressed == size);
        size_t picks[3] = {0, s2.count / 2, s2.count > 0 ? s2.count - 1 : 0};
        for (size_t i = 0; i < 3 && s2.count > 0; i++) {
            size_t k = picks[i];
            seen s3;
            memset(&s3, 0, sizeof s3);
            xc_zip_position end3;
            err = read_once(&src, &m, &s3, s2.cks[k], s2.ck_len[k], s2.pos[k].offset, false, &end3);
            check(err == XC_OK && end3.crc32 == m.crc32 && end3.offset == m.size);
        }
    } else {
        check(err == XC_ERR_CORRUPT || err == XC_ERR_ZIP_CRC);
    }

    /* 3 */
    if (s2.count > 0 && size > 0) {
        size_t len = s2.ck_len[0];
        size_t at = ((size_t)data[0] * 257U + size) % len;
        s2.cks[0][at] ^= (uint8_t)(data[size - 1] | 1U);
        xc_zip_position p;
        err = xc_zip_checkpoint_position(s2.cks[0], len, &m, &p);
        check(err == XC_ERR_CORRUPT || err == XC_ERR_UNSUPPORTED);
    }
    for (size_t i = 0; i < s2.count; i++) {
        free(s2.cks[i]);
    }
    return 0;
}
