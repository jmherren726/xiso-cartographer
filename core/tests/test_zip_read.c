/* Tests for reading zip members with checkpoints (xcat/zip.h, docs/formats/zip.md §9–§10). */
#include "unity.h"
#include "xcat/crc32.h"
#include "xcat/zip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

#define CK_EVERY ((size_t)2 * XC_ZIP_DICT_SIZE) /* 64 KiB: 11 checkpoints in the image */
#define MAX_CKS 64

static const char *fixtures_dir;

static uint8_t *slurp(const char *name, size_t *len)
{
    char path[1024];
    (void)snprintf(path, sizeof path, "%s/%s", fixtures_dir, name);
    FILE *f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    TEST_ASSERT_EQUAL_INT(0, fseek(f, 0, SEEK_END));
    long n = ftell(f);
    TEST_ASSERT_TRUE(n > 0 && n < 64L * 1024 * 1024); /* fixtures are small */
    TEST_ASSERT_EQUAL_INT(0, fseek(f, 0, SEEK_SET));
    uint8_t *buf = malloc((size_t)n);
    TEST_ASSERT_NOT_NULL(buf);
    TEST_ASSERT_EQUAL_size_t((size_t)n, fread(buf, 1, (size_t)n, f));
    TEST_ASSERT_EQUAL_INT(0, fclose(f));
    *len = (size_t)n;
    return buf;
}

/* --- the archive under test, and a source that can stream -------------------------- */

static uint8_t *iso; /* small.iso, loaded by the first open_zip() */
static size_t iso_len;
static uint8_t *zip;
static size_t zip_len;
static xc_mem_source mem;
static xc_source src;
static xc_zip_member member;
static size_t stream_piece; /* 0: the source has no read_stream */
static uint8_t *got;        /* what on_data received, iso_len bytes */

/* Streams from memory in pieces of 1, 2, ... stream_piece bytes, cycling. */
static xc_err mem_stream(void *ctx, uint64_t offset, uint64_t len, xc_source_data_fn fn,
                         void *fn_ctx)
{
    const xc_mem_source *m = ctx;
    TEST_ASSERT_TRUE(offset <= m->len && len <= m->len - offset);
    size_t piece = 1;
    while (len > 0) {
        size_t n = (uint64_t)piece < len ? piece : (size_t)len;
        xc_err err = fn(fn_ctx, offset, m->data + offset, n);
        if (err != XC_OK) {
            return err;
        }
        offset += n;
        len -= n;
        piece = piece % stream_piece + 1;
    }
    return XC_OK;
}

static void open_zip(const char *name)
{
    if (iso == NULL) {
        iso = slurp("small.iso", &iso_len);
        got = malloc(iso_len);
        TEST_ASSERT_NOT_NULL(got);
    }
    free(zip);
    zip = slurp(name, &zip_len);
    xc_source_memory(&src, &mem, zip, zip_len);
    if (stream_piece > 0) {
        src.read_stream = mem_stream;
    }
    xc_zip_listing l;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_zip_list(&src, &l));
    size_t i = 0;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_zip_pick_image(&l, &i));
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_zip_open_member(&src, &l, i, &member));
    xc_zip_listing_free(&l);
    TEST_ASSERT_EQUAL_UINT64(iso_len, member.size);
}

/* --- what the callbacks saw ------------------------------------------------------- */

static uint64_t got_from; /* first offset delivered */
static uint64_t got_next; /* next offset expected */
static uint64_t fail_at;  /* on_data fails once an offset reaches this; UINT64_MAX never */
static size_t ck_count;
static xc_zip_position ck_pos[MAX_CKS];
static uint8_t *ck_blob[MAX_CKS];
static size_t ck_len[MAX_CKS];
static xc_err ck_fail; /* returned by on_checkpoint when not XC_OK */

static xc_err on_data(void *ctx, uint64_t offset, const uint8_t *data, size_t len)
{
    (void)ctx;
    TEST_ASSERT_EQUAL_UINT64(got_next, offset); /* in order, each once */
    TEST_ASSERT_TRUE(len > 0 && offset + len <= iso_len);
    if (offset + len > fail_at) {
        return XC_ERR_CANCELLED;
    }
    memcpy(got + offset, data, len);
    got_next = offset + len;
    return XC_OK;
}

static xc_err on_checkpoint(void *ctx, const xc_zip_position *pos, const uint8_t *blob, size_t len)
{
    (void)ctx;
    if (ck_fail != XC_OK) {
        return ck_fail;
    }
    TEST_ASSERT_TRUE(ck_count < MAX_CKS);
    TEST_ASSERT_EQUAL_size_t(xc_zip_checkpoint_size(member.method), len);
    ck_pos[ck_count] = *pos;
    ck_blob[ck_count] = malloc(len);
    TEST_ASSERT_NOT_NULL(ck_blob[ck_count]);
    memcpy(ck_blob[ck_count], blob, len);
    ck_len[ck_count] = len;
    ck_count++;
    return XC_OK;
}

static void reset_seen(uint64_t from)
{
    memset(got, 0, iso_len);
    got_from = from;
    got_next = from;
    fail_at = UINT64_MAX;
    ck_fail = XC_OK;
}

static void free_checkpoints(void)
{
    for (size_t i = 0; i < ck_count; i++) {
        free(ck_blob[i]);
    }
    ck_count = 0;
}

static xc_zip_read request(void)
{
    xc_zip_read q;
    memset(&q, 0, sizeof q);
    q.src = &src;
    q.member = &member;
    q.on_data = on_data;
    q.read_size = 1000; /* odd pieces for sources without read_stream */
    return q;
}

/* Reads the member from `blob` (or 0) to the end and checks every byte. */
static void read_and_check(const uint8_t *blob, size_t blob_len, uint64_t from)
{
    reset_seen(from);
    xc_zip_read q = request();
    q.resume = blob;
    q.resume_len = blob_len;
    xc_zip_position end;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_zip_read_member(&q, &end));
    TEST_ASSERT_EQUAL_UINT64(iso_len, got_next);
    TEST_ASSERT_EQUAL_MEMORY(iso + from, got + from, iso_len - from);
    TEST_ASSERT_EQUAL_UINT64(iso_len, end.offset);
    TEST_ASSERT_EQUAL_UINT64(member.compressed_size, end.compressed);
    TEST_ASSERT_EQUAL_UINT32(member.crc32, end.crc32);
}

/* The whole member with checkpoints, then a resume from each of them. */
static void full_and_resumes(const char *zip_name)
{
    open_zip(zip_name);
    reset_seen(0);
    xc_zip_read q = request();
    q.checkpoint_every = CK_EVERY;
    q.on_checkpoint = on_checkpoint;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_zip_read_member(&q, NULL));
    TEST_ASSERT_EQUAL_MEMORY(iso, got, iso_len);
    TEST_ASSERT_EQUAL_size_t((iso_len - 1) / CK_EVERY, ck_count);
    for (size_t i = 0; i < ck_count; i++) {
        uint64_t off = (uint64_t)(i + 1) * CK_EVERY;
        TEST_ASSERT_EQUAL_UINT64(off, ck_pos[i].offset);
        TEST_ASSERT_EQUAL_UINT32(xc_crc32(0, iso, (size_t)off), ck_pos[i].crc32);
        TEST_ASSERT_TRUE(ck_pos[i].compressed <= member.compressed_size);
        xc_zip_position p;
        TEST_ASSERT_EQUAL_INT(XC_OK,
                              xc_zip_checkpoint_position(ck_blob[i], ck_len[i], &member, &p));
        TEST_ASSERT_EQUAL_UINT64(off, p.offset);
        TEST_ASSERT_EQUAL_UINT64(ck_pos[i].compressed, p.compressed);
    }
    for (size_t i = 0; i < ck_count; i++) {
        read_and_check(ck_blob[i], ck_len[i], ck_pos[i].offset);
    }
    free_checkpoints();
}

/* --- reading ---------------------------------------------------------------------- */

static void test_deflate_read_at(void)
{
    stream_piece = 0;
    full_and_resumes("small-deflate.zip");
}

static void test_deflate_streamed(void)
{
    stream_piece = 7919; /* pieces of 1 to 7919 bytes */
    full_and_resumes("small-deflate.zip");
    stream_piece = 0;
}

static void test_deflate_level0(void)
{
    full_and_resumes("small-deflate0.zip");
}

static void test_stored(void)
{
    full_and_resumes("small-stored.zip");
    stream_piece = 3;
    full_and_resumes("small-stored.zip");
    stream_piece = 0;
}

static void test_stop_early(void)
{
    open_zip("small-deflate.zip");
    reset_seen(0);
    xc_zip_read q = request();
    q.stop_at = 100000;
    q.checkpoint_every = CK_EVERY;
    q.on_checkpoint = on_checkpoint;
    xc_zip_position end;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_zip_read_member(&q, &end));
    TEST_ASSERT_EQUAL_UINT64(100000, got_next);
    TEST_ASSERT_EQUAL_UINT64(100000, end.offset);
    TEST_ASSERT_EQUAL_MEMORY(iso, got, 100000);
    TEST_ASSERT_EQUAL_size_t(1, ck_count); /* at 64 KiB, before the stop */
    /* pass 2: from that checkpoint to 300000, with the same CRC-32s as pass 1 */
    uint8_t *blob = ck_blob[0];
    size_t len = ck_len[0];
    ck_count = 0;
    reset_seen(CK_EVERY);
    q.resume = blob;
    q.resume_len = len;
    q.stop_at = 300000;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_zip_read_member(&q, &end));
    TEST_ASSERT_EQUAL_UINT64(300000, got_next);
    TEST_ASSERT_EQUAL_MEMORY(iso + CK_EVERY, got + CK_EVERY, 300000 - CK_EVERY);
    TEST_ASSERT_EQUAL_UINT32(xc_crc32(0, iso, 300000), end.crc32);
    TEST_ASSERT_EQUAL_size_t(3, ck_count); /* 128, 192 and 256 KiB */
    TEST_ASSERT_EQUAL_UINT32(xc_crc32(0, iso, 4U * CK_EVERY), ck_pos[2].crc32);
    free_checkpoints();
    free(blob);
}

/* --- bad data --------------------------------------------------------------------- */

static xc_err read_whole(void)
{
    reset_seen(0);
    xc_zip_read q = request();
    return xc_zip_read_member(&q, NULL);
}

static void test_damaged_member(void)
{
    open_zip("small-deflate.zip");
    xc_zip_member good = member;
    member.crc32 ^= 1U;
    TEST_ASSERT_EQUAL_INT(XC_ERR_ZIP_CRC, read_whole());
    member = good;
    member.size -= 1; /* more output than the size */
    TEST_ASSERT_EQUAL_INT(XC_ERR_CORRUPT, read_whole());
    member = good;
    member.size += 1; /* less output than the size */
    TEST_ASSERT_EQUAL_INT(XC_ERR_CORRUPT, read_whole());
    member = good;
    member.compressed_size -= 1; /* the data ends before its last block */
    TEST_ASSERT_EQUAL_INT(XC_ERR_CORRUPT, read_whole());
    member = good;
    member.compressed_size += 1; /* a byte after the last block */
    TEST_ASSERT_EQUAL_INT(XC_ERR_CORRUPT, read_whole());
    member = good;
    zip[member.data_offset + member.compressed_size / 2] ^= 0x5AU;
    xc_err err = read_whole();
    TEST_ASSERT_TRUE(err == XC_ERR_CORRUPT || err == XC_ERR_ZIP_CRC);
    zip[member.data_offset] = 0xFF; /* block type 3: invalid */
    TEST_ASSERT_EQUAL_INT(XC_ERR_CORRUPT, read_whole());
}

static void test_stored_damage(void)
{
    open_zip("small-stored.zip");
    zip[member.data_offset + 1000] ^= 1U;
    TEST_ASSERT_EQUAL_INT(XC_ERR_ZIP_CRC, read_whole());
    member.compressed_size -= 1;
    TEST_ASSERT_EQUAL_INT(XC_ERR_CORRUPT, read_whole()); /* sizes differ */
}

static void test_empty_deflate_member(void)
{
    static const uint8_t empty[] = {0x03, 0x00}; /* one final fixed block, end of block */
    xc_mem_source m;
    xc_source s;
    xc_source_memory(&s, &m, empty, sizeof empty);
    xc_zip_member e = {0, XC_ZIP_DEFLATE, 0, sizeof empty, 0, 0};
    xc_zip_read q = request();
    q.src = &s;
    q.member = &e;
    xc_zip_position end;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_zip_read_member(&q, &end));
    TEST_ASSERT_EQUAL_UINT64(0, end.offset);
    TEST_ASSERT_EQUAL_UINT64(2, end.compressed);
    e.compressed_size = 0;
    TEST_ASSERT_EQUAL_INT(XC_ERR_CORRUPT, xc_zip_read_member(&q, &end));
}

/* --- checkpoints ------------------------------------------------------------------ */

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8U);
    p[2] = (uint8_t)(v >> 16U);
    p[3] = (uint8_t)(v >> 24U);
}

/* Changes byte `at` of a copy of `blob`, fixing its trailing CRC-32 if `fix`. */
static uint8_t *altered(const uint8_t *blob, size_t len, size_t at, uint8_t value, bool fix)
{
    uint8_t *b = malloc(len);
    TEST_ASSERT_NOT_NULL(b);
    memcpy(b, blob, len);
    b[at] = value;
    if (fix) {
        put32(b + len - 4, xc_crc32(0, b, len - 4));
    }
    return b;
}

static void test_bad_checkpoints(void)
{
    open_zip("small-deflate.zip");
    reset_seen(0);
    xc_zip_read q = request();
    q.checkpoint_every = CK_EVERY;
    q.on_checkpoint = on_checkpoint;
    q.stop_at = 3U * CK_EVERY;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_zip_read_member(&q, NULL));
    TEST_ASSERT_EQUAL_size_t(2, ck_count);
    const uint8_t *b = ck_blob[1];
    size_t n = ck_len[1];
    xc_zip_position p;
    struct {
        size_t at;
        uint8_t value;
        bool fix;
        xc_err want;
    } cases[] = {
        {100, (uint8_t)(b[100] ^ 1U), false, XC_ERR_CORRUPT}, /* damaged */
        {0, 'Y', true, XC_ERR_UNSUPPORTED},                   /* magic */
        {4, (uint8_t)(b[4] ^ 1U), true, XC_ERR_UNSUPPORTED},  /* another build */
        {8, 0, true, XC_ERR_CORRUPT},                         /* method */
        {12, (uint8_t)(b[12] ^ 1U), true, XC_ERR_CORRUPT},    /* another member */
        {16, (uint8_t)(b[16] ^ 1U), true, XC_ERR_CORRUPT},    /* compressed size */
        {24, (uint8_t)(b[24] ^ 1U), true, XC_ERR_CORRUPT},    /* size */
        {32, (uint8_t)(b[32] ^ 1U), true, XC_ERR_CORRUPT},    /* not on a 32 KiB step */
        {39, 0x7F, true, XC_ERR_CORRUPT},                     /* offset past the end */
        {47, 0x7F, true, XC_ERR_CORRUPT},                     /* compressed past the end */
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        uint8_t *x = altered(b, n, cases[i].at, cases[i].value, cases[i].fix);
        TEST_ASSERT_EQUAL_INT_MESSAGE(cases[i].want, xc_zip_checkpoint_position(x, n, &member, &p),
                                      "case");
        q.resume = x;
        q.resume_len = n;
        q.stop_at = 0;
        TEST_ASSERT_EQUAL_INT(cases[i].want, xc_zip_read_member(&q, NULL));
        free(x);
    }
    TEST_ASSERT_EQUAL_INT(XC_ERR_CORRUPT, xc_zip_checkpoint_position(b, n - 1, &member, &p));
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_zip_checkpoint_position(NULL, n, &member, &p));
    /* a stored member's checkpoint doesn't fit a Deflate member */
    xc_zip_member stored = member;
    stored.method = XC_ZIP_STORED;
    TEST_ASSERT_EQUAL_INT(XC_ERR_CORRUPT, xc_zip_checkpoint_position(b, n, &stored, &p));
    free_checkpoints();
}

/* --- callbacks and arguments ------------------------------------------------------ */

static void test_callback_errors_stop_the_read(void)
{
    open_zip("small-deflate.zip");
    reset_seen(0);
    fail_at = 200000;
    xc_zip_read q = request();
    TEST_ASSERT_EQUAL_INT(XC_ERR_CANCELLED, xc_zip_read_member(&q, NULL));
    TEST_ASSERT_TRUE(got_next <= 200000);
    reset_seen(0);
    ck_fail = XC_ERR_IO;
    q.checkpoint_every = CK_EVERY;
    q.on_checkpoint = on_checkpoint;
    TEST_ASSERT_EQUAL_INT(XC_ERR_IO, xc_zip_read_member(&q, NULL));
    TEST_ASSERT_EQUAL_UINT64(CK_EVERY, got_next);
    stream_piece = 4096;
    open_zip("small-deflate.zip");
    reset_seen(0);
    fail_at = 50000;
    q = request();
    TEST_ASSERT_EQUAL_INT(XC_ERR_CANCELLED, xc_zip_read_member(&q, NULL));
    stream_piece = 0;
}

static void test_invalid_arguments(void)
{
    open_zip("small-deflate.zip");
    xc_zip_read q = request();
    q.checkpoint_every = 1000; /* not a multiple of 32 KiB */
    q.on_checkpoint = on_checkpoint;
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_zip_read_member(&q, NULL));
    q = request();
    q.checkpoint_every = CK_EVERY; /* no callback */
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_zip_read_member(&q, NULL));
    q = request();
    q.on_data = NULL;
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_zip_read_member(&q, NULL));
    xc_zip_member other = member;
    other.method = 9;
    q = request();
    q.member = &other;
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_zip_read_member(&q, NULL));
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_zip_read_member(NULL, NULL));
    TEST_ASSERT_EQUAL_size_t(0, xc_zip_checkpoint_size(9));
    TEST_ASSERT_EQUAL_size_t(60, xc_zip_checkpoint_size(XC_ZIP_STORED));
    TEST_ASSERT_TRUE(xc_zip_checkpoint_size(XC_ZIP_DEFLATE) > 32768U + 60U);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        (void)fprintf(stderr, "usage: test_zip_read <fixtures-dir>\n");
        return 2;
    }
    fixtures_dir = argv[1];
    UNITY_BEGIN();
    RUN_TEST(test_deflate_read_at);
    RUN_TEST(test_deflate_streamed);
    RUN_TEST(test_deflate_level0);
    RUN_TEST(test_stored);
    RUN_TEST(test_stop_early);
    RUN_TEST(test_damaged_member);
    RUN_TEST(test_stored_damage);
    RUN_TEST(test_empty_deflate_member);
    RUN_TEST(test_bad_checkpoints);
    RUN_TEST(test_callback_errors_stop_the_read);
    RUN_TEST(test_invalid_arguments);
    int rc = UNITY_END();
    free(got);
    free(iso);
    free(zip);
    return rc;
}
