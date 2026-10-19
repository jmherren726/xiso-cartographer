/*
 * Reading a zip member's data, stored or Deflate, with checkpoints: docs/formats/zip.md
 * §9–§10. Deflate goes through miniz's tinfl (third_party/miniz, cmake/miniz.cmake).
 */
#include "xcat/zip.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "bytes.h"
#include "miniz.h"
#include "xcat/crc32.h"

#define DICT XC_ZIP_DICT_SIZE
#define CK_HEAD 56U
#define CK_MAGIC 0x315A4358U /* "XCZ1" */
#define CK_TAG ((uint32_t)sizeof(tinfl_decompressor) ^ ((uint32_t)MZ_VERNUM << 16U))
#define DEFAULT_READ_SIZE ((size_t)64 * 1024)

typedef struct reader {
    const xc_zip_read *req;
    const xc_zip_member *m;
    uint64_t stop;    /* stop delivering here (< size), or the member's size */
    bool early;       /* stop < size: no CRC check, the rest isn't read */
    uint64_t next_ck; /* the next checkpoint offset; UINT64_MAX for none */
    xc_zip_position pos;
    bool done;    /* the member is complete */
    bool stopped; /* reached `stop` early */
    xc_err err;   /* first error, ours or a callback's */
    size_t dict_ofs;
    uint8_t *ck; /* checkpoint buffer, xc_zip_checkpoint_size() bytes */
    tinfl_decompressor inf;
    uint8_t dict[DICT];
} reader;

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8U);
    p[2] = (uint8_t)(v >> 16U);
    p[3] = (uint8_t)(v >> 24U);
}

static void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32U));
}

size_t xc_zip_checkpoint_size(uint16_t method)
{
    if (method == XC_ZIP_STORED) {
        return CK_HEAD + 4U;
    }
    if (method == XC_ZIP_DEFLATE) {
        return CK_HEAD + sizeof(tinfl_decompressor) + DICT + 4U;
    }
    return 0;
}

xc_err xc_zip_checkpoint_position(const uint8_t *blob, size_t len, const xc_zip_member *member,
                                  xc_zip_position *pos)
{
    if (blob == NULL || member == NULL || pos == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    size_t want = xc_zip_checkpoint_size(member->method);
    uint32_t magic = 0;
    uint32_t tag = 0;
    uint32_t sum = 0;
    if (want == 0 || len != want || !xc_rd_u32le(blob, len, len - 4U, &sum) ||
        sum != xc_crc32(0, blob, len - 4U)) {
        return XC_ERR_CORRUPT;
    }
    (void)xc_rd_u32le(blob, len, 0, &magic);
    (void)xc_rd_u32le(blob, len, 4, &tag);
    if (magic != CK_MAGIC || tag != CK_TAG) {
        return XC_ERR_UNSUPPORTED; /* another build's checkpoint: start again from 0 */
    }
    uint16_t method = 0;
    uint32_t crc = 0;
    uint64_t csize = 0;
    uint64_t size = 0;
    xc_zip_position p;
    (void)xc_rd_u16le(blob, len, 8, &method);
    (void)xc_rd_u32le(blob, len, 12, &crc);
    (void)xc_rd_u64le(blob, len, 16, &csize);
    (void)xc_rd_u64le(blob, len, 24, &size);
    (void)xc_rd_u64le(blob, len, 32, &p.offset);
    (void)xc_rd_u64le(blob, len, 40, &p.compressed);
    (void)xc_rd_u32le(blob, len, 48, &p.crc32);
    if (method != member->method || crc != member->crc32 || csize != member->compressed_size ||
        size != member->size || p.offset > size || p.compressed > csize || p.offset % DICT != 0 ||
        (method == XC_ZIP_STORED && p.offset != p.compressed)) {
        return XC_ERR_CORRUPT;
    }
    *pos = p;
    return XC_OK;
}

/* §10: writes the checkpoint for the current position into r->ck and hands it over. */
static xc_err emit_checkpoint(reader *r)
{
    size_t len = xc_zip_checkpoint_size(r->m->method);
    uint8_t *b = r->ck;
    memset(b, 0, CK_HEAD);
    put32(b, CK_MAGIC);
    put32(b + 4, CK_TAG);
    b[8] = (uint8_t)r->m->method;
    b[9] = (uint8_t)(r->m->method >> 8U);
    put32(b + 12, r->m->crc32);
    put64(b + 16, r->m->compressed_size);
    put64(b + 24, r->m->size);
    put64(b + 32, r->pos.offset);
    put64(b + 40, r->pos.compressed);
    put32(b + 48, r->pos.crc32);
    if (r->m->method == XC_ZIP_DEFLATE) {
        memcpy(b + CK_HEAD, &r->inf, sizeof r->inf);
        memcpy(b + CK_HEAD + sizeof r->inf, r->dict, DICT);
    }
    put32(b + len - 4U, xc_crc32(0, b, len - 4U));
    return r->req->on_checkpoint(r->req->checkpoint_ctx, &r->pos, b, len);
}

/* After output has advanced: a checkpoint if one is due. */
static bool maybe_checkpoint(reader *r)
{
    if (r->pos.offset != r->next_ck || r->pos.offset >= r->m->size || r->stopped) {
        return true;
    }
    r->next_ck += r->req->checkpoint_every;
    xc_err err = emit_checkpoint(r);
    if (err != XC_OK) {
        r->err = err;
        return false;
    }
    return true;
}

/* Uncompressed bytes out: checked against the size, counted in the CRC, delivered up to
   `stop`. False if the read must end (error, or `stop` reached). */
static bool deliver(reader *r, const uint8_t *data, size_t n)
{
    if (n > r->m->size - r->pos.offset) {
        r->err = XC_ERR_CORRUPT; /* more bytes than the member's size */
        return false;
    }
    uint64_t room = r->stop - r->pos.offset;
    size_t give = (uint64_t)n < room ? n : (size_t)room;
    if (give > 0) {
        xc_err err = r->req->on_data(r->req->data_ctx, r->pos.offset, data, give);
        if (err != XC_OK) {
            r->err = err;
            return false;
        }
    }
    r->pos.crc32 = xc_crc32(r->pos.crc32, data, give);
    r->pos.offset += give;
    if (r->early && r->pos.offset >= r->stop) {
        r->stopped = true;
        return false;
    }
    return true;
}

/* Stored: the compressed bytes are the image bytes. */
static void feed_stored(reader *r, const uint8_t *data, size_t n)
{
    while (n > 0 && r->err == XC_OK && !r->stopped) {
        size_t piece = n;
        if (r->next_ck != UINT64_MAX && r->next_ck - r->pos.offset < piece) {
            piece = (size_t)(r->next_ck - r->pos.offset);
        }
        if (!deliver(r, data, piece)) {
            return;
        }
        r->pos.compressed += piece;
        data += piece;
        n -= piece;
        if (!maybe_checkpoint(r)) {
            return;
        }
    }
    if (r->pos.compressed == r->m->compressed_size) {
        r->done = true;
    }
}

/* Deflate: through tinfl, a 32 KiB wrapping dictionary at a time. */
static void feed_deflate(reader *r, const uint8_t *data, size_t n)
{
    for (;;) {
        if (r->done) {
            if (n > 0) {
                r->err = XC_ERR_CORRUPT; /* bytes after the last block */
            }
            return;
        }
        size_t in_sz = n;
        size_t out_sz = DICT - r->dict_ofs;
        bool more = r->pos.compressed + n < r->m->compressed_size;
        tinfl_status st = tinfl_decompress(&r->inf, data, &in_sz, r->dict, r->dict + r->dict_ofs,
                                           &out_sz, more ? TINFL_FLAG_HAS_MORE_INPUT : 0);
        data += in_sz;
        n -= in_sz;
        r->pos.compressed += in_sz;
        if (out_sz > 0) {
            if (!deliver(r, r->dict + r->dict_ofs, out_sz)) {
                return;
            }
            r->dict_ofs = (r->dict_ofs + out_sz) & (DICT - 1U);
        }
        if (st < TINFL_STATUS_DONE) {
            r->err = XC_ERR_CORRUPT; /* invalid data, or it ends before its last block */
            return;
        }
        if (st == TINFL_STATUS_DONE) {
            r->done = true;
            continue;
        }
        if (!maybe_checkpoint(r)) {
            return;
        }
        if (st == TINFL_STATUS_NEEDS_MORE_INPUT && n == 0) {
            return;
        }
    }
}

/* One piece of compressed input. False when the read is over (done, stopped or failed). */
static bool feed(reader *r, const uint8_t *data, size_t n)
{
    if (r->m->method == XC_ZIP_STORED) {
        feed_stored(r, data, n);
    } else {
        feed_deflate(r, data, n);
    }
    return r->err == XC_OK && !r->stopped;
}

static xc_err on_stream(void *ctx, uint64_t offset, const uint8_t *data, size_t len)
{
    (void)offset; /* the source delivers the requested range in order */
    reader *r = ctx;
    if (!feed(r, data, len)) {
        return r->err != XC_OK ? r->err : XC_ERR_CANCELLED;
    }
    return XC_OK;
}

/* Feeds the compressed bytes from the current position to the member's end. */
static void run(reader *r)
{
    const xc_source *src = r->req->src;
    uint64_t at = r->m->data_offset + r->pos.compressed;
    uint64_t left = r->m->compressed_size - r->pos.compressed;
    if (left == 0) {
        static const uint8_t none[1] = {0};
        (void)feed(r, none, 0);
        return;
    }
    if (src->read_stream != NULL) {
        xc_err err = src->read_stream(src->ctx, at, left, on_stream, r);
        if (err != XC_OK && r->err == XC_OK && !r->stopped) {
            r->err = err;
        }
        return;
    }
    size_t size = r->req->read_size > 0 ? r->req->read_size : DEFAULT_READ_SIZE;
    uint8_t *buf = malloc(size);
    if (buf == NULL) {
        r->err = XC_ERR_NOMEM;
        return;
    }
    while (left > 0) {
        size_t n = left < (uint64_t)size ? (size_t)left : size;
        xc_err err = xc_source_read(src, at, buf, n);
        if (err != XC_OK) {
            r->err = err;
            break;
        }
        at += n;
        left -= n;
        if (!feed(r, buf, n)) {
            break;
        }
    }
    free(buf);
}

/* Restores the reader from a checkpoint (§10). */
static xc_err resume(reader *r, const uint8_t *blob, size_t len)
{
    xc_err err = xc_zip_checkpoint_position(blob, len, r->m, &r->pos);
    if (err != XC_OK) {
        return err;
    }
    if (r->m->method == XC_ZIP_DEFLATE) {
        memcpy(&r->inf, blob + CK_HEAD, sizeof r->inf);
        memcpy(r->dict, blob + CK_HEAD + sizeof r->inf, DICT);
    }
    r->dict_ofs = 0; /* offsets are multiples of the dictionary size */
    return XC_OK;
}

xc_err xc_zip_read_member(const xc_zip_read *req, xc_zip_position *end)
{
    if (req == NULL || req->src == NULL || req->member == NULL || req->on_data == NULL ||
        (req->member->method != XC_ZIP_STORED && req->member->method != XC_ZIP_DEFLATE) ||
        req->checkpoint_every % DICT != 0 ||
        (req->checkpoint_every > 0 && req->on_checkpoint == NULL)) {
        return XC_ERR_INVALID_ARG;
    }
    const xc_zip_member *m = req->member;
    if (m->method == XC_ZIP_STORED && m->compressed_size != m->size) {
        return XC_ERR_CORRUPT;
    }
    reader *r = malloc(sizeof *r);
    uint8_t *ck = req->checkpoint_every > 0 ? malloc(xc_zip_checkpoint_size(m->method)) : NULL;
    if (r == NULL || (req->checkpoint_every > 0 && ck == NULL)) {
        free(r);
        free(ck);
        return XC_ERR_NOMEM;
    }
    memset(r, 0, offsetof(reader, inf));
    r->req = req;
    r->m = m;
    r->ck = ck;
    r->err = XC_OK;
    tinfl_init(&r->inf);
    /* tinfl's wrapping mode doesn't reject distances reaching before the first byte, so
       such (invalid) data reads zeros, never stale memory; the CRC-32 then catches it. */
    memset(r->dict, 0, sizeof r->dict);
    xc_err err = req->resume != NULL ? resume(r, req->resume, req->resume_len) : XC_OK;
    if (err == XC_OK) {
        r->early = req->stop_at > 0 && req->stop_at < m->size;
        r->stop = r->early ? req->stop_at : m->size;
        r->next_ck = req->checkpoint_every > 0 ? r->pos.offset + req->checkpoint_every : UINT64_MAX;
        if (r->pos.offset < r->stop || !r->early) {
            run(r);
        } else {
            r->stopped = true;
        }
        err = r->err;
    }
    if (err == XC_OK && !r->stopped) {
        /* §9.3: the whole member, exactly, with its CRC-32 */
        if (!r->done || r->pos.offset != m->size || r->pos.compressed != m->compressed_size) {
            err = XC_ERR_CORRUPT;
        } else if (r->pos.crc32 != m->crc32) {
            err = XC_ERR_ZIP_CRC;
        }
    }
    if (end != NULL) {
        *end = r->pos;
    }
    free(ck);
    free(r);
    return err;
}
