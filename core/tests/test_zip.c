/* Tests for the zip directory reader (xcat/zip.h, docs/formats/zip.md). */
#include "unity.h"
#include "xcat/crc32.h"
#include "xcat/zip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* --- a small zip writer ---------------------------------------------------------- */

static uint8_t z[1U << 20U];
static size_t zlen;

static void put8(uint8_t v)
{
    TEST_ASSERT_TRUE(zlen < sizeof z);
    z[zlen++] = v;
}

static void put16(uint16_t v)
{
    put8((uint8_t)v);
    put8((uint8_t)(v >> 8U));
}

static void put32(uint32_t v)
{
    put16((uint16_t)v);
    put16((uint16_t)(v >> 16U));
}

static void put64(uint64_t v)
{
    put32((uint32_t)v);
    put32((uint32_t)(v >> 32U));
}

static void put_bytes(const void *p, size_t n)
{
    TEST_ASSERT_TRUE(n <= sizeof z - zlen);
    memcpy(z + zlen, p, n);
    zlen += n;
}

static void set16(size_t at, uint16_t v)
{
    z[at] = (uint8_t)v;
    z[at + 1] = (uint8_t)(v >> 8U);
}

static void set32(size_t at, uint32_t v)
{
    set16(at, (uint16_t)v);
    set16(at + 2, (uint16_t)(v >> 16U));
}

typedef struct ent {
    const char *name;
    uint16_t method;
    uint16_t flags;
    const char *data; /* the member's bytes as stored */
    bool zip64;       /* all-ones sizes and offset in the central directory, Zip64 extra */
    /* filled by build() */
    size_t local_at;
    size_t data_at;
    size_t central_at;
} ent;

typedef struct arc {
    bool zip64_end; /* Zip64 end record and locator; all-ones EOCD fields */
    const char *comment;
    /* filled by build() */
    size_t cd_at;
    size_t cd_len;
    size_t eocd_at;
} arc;

static void build(arc *a, ent *e, size_t n)
{
    zlen = 0;
    for (size_t i = 0; i < n; i++) {
        size_t len = strlen(e[i].data);
        uint16_t name_len = (uint16_t)strlen(e[i].name);
        e[i].local_at = zlen;
        put32(0x04034b50U);
        put16(20);
        put16(e[i].flags);
        put16(e[i].method);
        put32(0);
        put32(xc_crc32(0, e[i].data, len));
        put32((uint32_t)len);
        put32((uint32_t)len);
        put16(name_len);
        put16(0);
        put_bytes(e[i].name, name_len);
        e[i].data_at = zlen;
        put_bytes(e[i].data, len);
    }
    a->cd_at = zlen;
    for (size_t i = 0; i < n; i++) {
        size_t len = strlen(e[i].data);
        uint16_t name_len = (uint16_t)strlen(e[i].name);
        e[i].central_at = zlen;
        put32(0x02014b50U);
        put16(45);
        put16(45);
        put16(e[i].flags);
        put16(e[i].method);
        put32(0);
        put32(xc_crc32(0, e[i].data, len));
        put32(e[i].zip64 ? 0xFFFFFFFFU : (uint32_t)len);
        put32(e[i].zip64 ? 0xFFFFFFFFU : (uint32_t)len);
        put16(name_len);
        put16(e[i].zip64 ? 4 + 24 : 0);
        put16(0);
        put16(0);
        put16(0);
        put32(0);
        put32(e[i].zip64 ? 0xFFFFFFFFU : (uint32_t)e[i].local_at);
        put_bytes(e[i].name, name_len);
        if (e[i].zip64) {
            put16(1);
            put16(24);
            put64(len); /* uncompressed */
            put64(len); /* compressed */
            put64(e[i].local_at);
        }
    }
    a->cd_len = zlen - a->cd_at;
    if (a->zip64_end) {
        size_t rec = zlen;
        put32(0x06064b50U);
        put64(44);
        put16(45);
        put16(45);
        put32(0);
        put32(0);
        put64(n);
        put64(n);
        put64(a->cd_len);
        put64(a->cd_at);
        put32(0x07064b50U);
        put32(0);
        put64(rec);
        put32(1);
    }
    a->eocd_at = zlen;
    size_t clen = a->comment != NULL ? strlen(a->comment) : 0;
    put32(0x06054b50U);
    put16(0);
    put16(0);
    put16(a->zip64_end ? 0xFFFFU : (uint16_t)n);
    put16(a->zip64_end ? 0xFFFFU : (uint16_t)n);
    put32(a->zip64_end ? 0xFFFFFFFFU : (uint32_t)a->cd_len);
    put32(a->zip64_end ? 0xFFFFFFFFU : (uint32_t)a->cd_at);
    put16((uint16_t)clen);
    put_bytes(a->comment != NULL ? a->comment : "", clen);
}

static xc_mem_source mem;
static xc_source src;

static xc_err list(xc_zip_listing *l)
{
    xc_source_memory(&src, &mem, z, zlen);
    return xc_zip_list(&src, l);
}

static void expect_list_error(xc_err want)
{
    xc_zip_listing l;
    TEST_ASSERT_EQUAL_INT(want, list(&l));
    TEST_ASSERT_NULL(l.entries);
    TEST_ASSERT_EQUAL_size_t(0, l.count);
}

/* Lists, picks the image and opens it; returns the open result. */
static xc_err open_image(xc_zip_member *m)
{
    xc_zip_listing l;
    xc_err err = list(&l);
    if (err == XC_OK) {
        size_t i = 0;
        err = xc_zip_pick_image(&l, &i);
        if (err == XC_OK) {
            err = xc_zip_open_member(&src, &l, i, m);
        }
        xc_zip_listing_free(&l);
    }
    return err;
}

/* --- listing (§2–§4) ------------------------------------------------------------- */

static void test_one_stored_member(void)
{
    ent e[] = {{"game.iso", XC_ZIP_STORED, 0, "hello image", false, 0, 0, 0}};
    arc a = {false, NULL, 0, 0, 0};
    build(&a, e, 1);
    xc_zip_listing l;
    TEST_ASSERT_EQUAL_INT(XC_OK, list(&l));
    TEST_ASSERT_EQUAL_size_t(1, l.count);
    TEST_ASSERT_FALSE(l.zip64);
    TEST_ASSERT_EQUAL_UINT64(a.cd_at, l.cd_offset);
    TEST_ASSERT_EQUAL_STRING("game.iso", l.entries[0].name);
    TEST_ASSERT_EQUAL_UINT16(0, l.entries[0].method);
    TEST_ASSERT_EQUAL_UINT32(xc_crc32(0, "hello image", 11), l.entries[0].crc32);
    TEST_ASSERT_EQUAL_UINT64(11, l.entries[0].size);
    TEST_ASSERT_EQUAL_UINT64(11, l.entries[0].compressed_size);
    TEST_ASSERT_EQUAL_UINT64(0, l.entries[0].local_offset);
    xc_zip_member m = {0, 0, 0, 0, 0, 0};
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_zip_open_member(&src, &l, 0, &m));
    TEST_ASSERT_EQUAL_UINT64(e[0].data_at, m.data_offset);
    TEST_ASSERT_EQUAL_UINT64(11, m.size);
    TEST_ASSERT_EQUAL_MEMORY("hello image", z + m.data_offset, 11);
    xc_zip_listing_free(&l);
    TEST_ASSERT_NULL(l.entries);
    xc_zip_listing_free(&l); /* twice is fine */
    xc_zip_listing_free(NULL);
}

static void test_zip64_records_and_extra(void)
{
    ent e[] = {{"readme.txt", XC_ZIP_STORED, 0, "text", true, 0, 0, 0},
               {"Game.XISO", XC_ZIP_DEFLATE, 0x0002, "deflated bytes", true, 0, 0, 0}};
    arc a = {true, NULL, 0, 0, 0};
    build(&a, e, 2);
    xc_zip_listing l;
    TEST_ASSERT_EQUAL_INT(XC_OK, list(&l));
    TEST_ASSERT_TRUE(l.zip64);
    TEST_ASSERT_EQUAL_size_t(2, l.count);
    TEST_ASSERT_EQUAL_UINT64(a.cd_at, l.cd_offset);
    TEST_ASSERT_EQUAL_UINT64(14, l.entries[1].size);
    TEST_ASSERT_EQUAL_UINT64(14, l.entries[1].compressed_size);
    TEST_ASSERT_EQUAL_UINT64(e[1].local_at, l.entries[1].local_offset);
    size_t i = 99;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_zip_pick_image(&l, &i));
    TEST_ASSERT_EQUAL_size_t(1, i);
    xc_zip_member m = {0, 0, 0, 0, 0, 0};
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_zip_open_member(&src, &l, i, &m));
    TEST_ASSERT_EQUAL_UINT16(XC_ZIP_DEFLATE, m.method);
    TEST_ASSERT_EQUAL_UINT64(e[1].data_at, m.data_offset);
    xc_zip_listing_free(&l);
}

static void test_comment_with_fake_signature(void)
{
    /* "PK\5\6" inside the comment must not be taken for the EOCD (§2.1) */
    ent e[] = {{"a.iso", XC_ZIP_STORED, 0, "x", false, 0, 0, 0}};
    arc a = {false, "comment PK\x05\x06 and more", 0, 0, 0};
    build(&a, e, 1);
    xc_zip_listing l;
    TEST_ASSERT_EQUAL_INT(XC_OK, list(&l));
    TEST_ASSERT_EQUAL_size_t(1, l.count);
    xc_zip_listing_free(&l);
}

static void test_longest_comment(void)
{
    ent e[] = {{"a.iso", XC_ZIP_STORED, 0, "x", false, 0, 0, 0}};
    static char comment[65536];
    memset(comment, 'c', 65535);
    comment[65535] = '\0';
    arc a = {true, comment, 0, 0, 0}; /* the locator is just inside the tail read */
    build(&a, e, 1);
    xc_zip_listing l;
    TEST_ASSERT_EQUAL_INT(XC_OK, list(&l));
    TEST_ASSERT_TRUE(l.zip64);
    xc_zip_listing_free(&l);
}

static void test_empty_zip(void)
{
    arc a = {false, NULL, 0, 0, 0};
    build(&a, NULL, 0);
    xc_zip_listing l;
    TEST_ASSERT_EQUAL_INT(XC_OK, list(&l));
    TEST_ASSERT_EQUAL_size_t(0, l.count);
    size_t i = 0;
    TEST_ASSERT_EQUAL_INT(XC_ERR_ZIP_MEMBER, xc_zip_pick_image(&l, &i));
    xc_zip_listing_free(&l);
}

static void test_not_zip(void)
{
    zlen = 0;
    expect_list_error(XC_ERR_NOT_ZIP);
    memset(z, 'A', 5000);
    zlen = 5000;
    expect_list_error(XC_ERR_NOT_ZIP);
    /* a signature whose comment length doesn't reach the end */
    set32(4000, 0x06054b50U);
    set16(4020, 7);
    expect_list_error(XC_ERR_NOT_ZIP);
}

static void test_split_archive_unsupported(void)
{
    ent e[] = {{"a.iso", XC_ZIP_STORED, 0, "x", false, 0, 0, 0}};
    arc a = {false, NULL, 0, 0, 0};
    build(&a, e, 1);
    set16(a.eocd_at + 4, 1); /* this disk */
    expect_list_error(XC_ERR_ZIP_UNSUPPORTED);
    build(&a, e, 1);
    set16(a.eocd_at + 8, 0); /* entries on this disk != total */
    expect_list_error(XC_ERR_ZIP_UNSUPPORTED);
    a.zip64_end = true;
    build(&a, e, 1);
    set32(a.eocd_at - 20 + 16, 2); /* locator: two disks */
    expect_list_error(XC_ERR_ZIP_UNSUPPORTED);
    build(&a, e, 1);
    set32(a.eocd_at - 20 - 56 + 20, 1); /* Zip64 end record: CD on disk 1 */
    expect_list_error(XC_ERR_ZIP_UNSUPPORTED);
}

static void test_bad_zip64_end_is_corrupt(void)
{
    ent e[] = {{"a.iso", XC_ZIP_STORED, 0, "x", false, 0, 0, 0}};
    arc a = {true, NULL, 0, 0, 0};
    build(&a, e, 1);
    set32(a.eocd_at - 20 - 56, 0x12345678U); /* record signature */
    expect_list_error(XC_ERR_CORRUPT);
    build(&a, e, 1);
    set32(a.eocd_at - 20 + 8, (uint32_t)(a.eocd_at - 20 - 10)); /* record overlaps locator */
    expect_list_error(XC_ERR_CORRUPT);
    build(&a, e, 1);
    set32(a.eocd_at - 20 - 56 + 4, 40); /* rest size < 44 */
    expect_list_error(XC_ERR_CORRUPT);
    /* all-ones EOCD fields with no locator (§3.2) */
    build(&a, e, 1);
    set32(a.eocd_at - 20, 0);
    expect_list_error(XC_ERR_CORRUPT);
}

static void test_central_directory_bounds(void)
{
    ent e[] = {{"a.iso", XC_ZIP_STORED, 0, "x", false, 0, 0, 0}};
    arc a = {false, NULL, 0, 0, 0};
    build(&a, e, 1);
    set32(a.eocd_at + 12, (uint32_t)a.cd_len + 1); /* runs into the EOCD */
    expect_list_error(XC_ERR_CORRUPT);
    build(&a, e, 1);
    set32(a.eocd_at + 16, 0xFFFFFFF0U); /* offset past the end */
    expect_list_error(XC_ERR_CORRUPT);
    build(&a, e, 1);
    set16(a.eocd_at + 8, 2); /* two entries promised, room for one */
    set16(a.eocd_at + 10, 2);
    expect_list_error(XC_ERR_CORRUPT);
    build(&a, e, 1);
    set32(a.cd_at, 0x02014b51U); /* header signature */
    expect_list_error(XC_ERR_CORRUPT);
    build(&a, e, 1);
    set16(e[0].central_at + 28, 200); /* name past the directory */
    expect_list_error(XC_ERR_CORRUPT);
    build(&a, e, 1);
    z[e[0].central_at + 46 + 1] = '\0'; /* NUL in the name */
    expect_list_error(XC_ERR_CORRUPT);
    build(&a, e, 1);
    set32(e[0].central_at + 42, (uint32_t)a.cd_at); /* local header at the CD */
    expect_list_error(XC_ERR_CORRUPT);
}

static void test_limits(void)
{
    ent e[] = {{"a.iso", XC_ZIP_STORED, 0, "x", false, 0, 0, 0}};
    arc a = {false, NULL, 0, 0, 0};
    build(&a, e, 1);
    set16(a.eocd_at + 8, 20000);
    set16(a.eocd_at + 10, 20000);
    expect_list_error(XC_ERR_LIMIT);
}

static void test_extra_fields(void)
{
    ent e[] = {{"a.iso", XC_ZIP_STORED, 0, "xyz", true, 0, 0, 0}};
    arc a = {false, NULL, 0, 0, 0};
    size_t extra;
    build(&a, e, 1);
    extra = e[0].central_at + 46 + 5;
    set16(extra + 2, 16); /* Zip64 field too short for three values */
    expect_list_error(XC_ERR_CORRUPT);
    build(&a, e, 1);
    set16(extra, 0x5455); /* not a Zip64 field: the all-ones values stay unreplaced */
    expect_list_error(XC_ERR_CORRUPT);
    build(&a, e, 1);
    set16(extra + 2, 26); /* field runs past the extra area */
    expect_list_error(XC_ERR_CORRUPT);
    build(&a, e, 1);
    set32(e[0].central_at + 20, 3); /* compressed size not all ones: field holds 2 values */
    xc_zip_listing l;
    TEST_ASSERT_EQUAL_INT(XC_OK, list(&l));
    TEST_ASSERT_EQUAL_UINT64(3, l.entries[0].compressed_size);
    TEST_ASSERT_EQUAL_UINT64(3, l.entries[0].size);
    /* ...the second value (the "compressed size" written) is now read as the offset: 3 */
    TEST_ASSERT_EQUAL_UINT64(3, l.entries[0].local_offset);
    xc_zip_listing_free(&l);
    build(&a, e, 1);
    set16(e[0].central_at + 34, 0xFFFFU); /* disk number with no room for it in the field */
    expect_list_error(XC_ERR_CORRUPT);
}

static void test_disk_number_unsupported(void)
{
    ent e[] = {{"a.iso", XC_ZIP_STORED, 0, "x", false, 0, 0, 0}};
    arc a = {false, NULL, 0, 0, 0};
    build(&a, e, 1);
    set16(e[0].central_at + 34, 1);
    expect_list_error(XC_ERR_ZIP_UNSUPPORTED);
}

/* --- choosing the image (§5) ----------------------------------------------------- */

static void test_image_names(void)
{
    TEST_ASSERT_TRUE(xc_zip_is_image_name("a.iso"));
    TEST_ASSERT_TRUE(xc_zip_is_image_name("dir/Game (USA).ISO"));
    TEST_ASSERT_TRUE(xc_zip_is_image_name("g.xiso"));
    TEST_ASSERT_TRUE(xc_zip_is_image_name("g.xiso.iso"));
    TEST_ASSERT_TRUE(xc_zip_is_image_name(".iso"));
    TEST_ASSERT_FALSE(xc_zip_is_image_name("a.iso/"));
    TEST_ASSERT_FALSE(xc_zip_is_image_name("a.iso.txt"));
    TEST_ASSERT_FALSE(xc_zip_is_image_name("iso"));
    TEST_ASSERT_FALSE(xc_zip_is_image_name("a.cso"));
    TEST_ASSERT_FALSE(xc_zip_is_image_name(NULL));
}

static void test_pick_none_or_several(void)
{
    ent e[] = {{"a.iso", XC_ZIP_STORED, 0, "x", false, 0, 0, 0},
               {"b.xiso", XC_ZIP_STORED, 0, "y", false, 0, 0, 0},
               {"c.txt", XC_ZIP_STORED, 0, "z", false, 0, 0, 0}};
    arc a = {false, NULL, 0, 0, 0};
    xc_zip_listing l;
    size_t i = 0;
    build(&a, e, 3);
    TEST_ASSERT_EQUAL_INT(XC_OK, list(&l));
    TEST_ASSERT_EQUAL_INT(XC_ERR_ZIP_MEMBER, xc_zip_pick_image(&l, &i));
    xc_zip_listing_free(&l);
    build(&a, e + 2, 1);
    TEST_ASSERT_EQUAL_INT(XC_OK, list(&l));
    TEST_ASSERT_EQUAL_INT(XC_ERR_ZIP_MEMBER, xc_zip_pick_image(&l, &i));
    xc_zip_listing_free(&l);
}

/* --- the member (§6) ------------------------------------------------------------- */

static void test_unsupported_members(void)
{
    static const uint16_t methods[] = {9, 14, 12, 99, 1};
    xc_zip_member m = {0, 0, 0, 0, 0, 0};
    for (size_t k = 0; k < sizeof methods / sizeof methods[0]; k++) {
        ent e[] = {{"a.iso", methods[k], 0, "x", false, 0, 0, 0}};
        arc a = {false, NULL, 0, 0, 0};
        build(&a, e, 1);
        TEST_ASSERT_EQUAL_INT(XC_ERR_ZIP_UNSUPPORTED, open_image(&m));
    }
    static const uint16_t flags[] = {0x0001, 0x0040, 0x2000};
    for (size_t k = 0; k < sizeof flags / sizeof flags[0]; k++) {
        ent e[] = {{"a.iso", XC_ZIP_DEFLATE, flags[k], "x", false, 0, 0, 0}};
        arc a = {false, NULL, 0, 0, 0};
        build(&a, e, 1);
        TEST_ASSERT_EQUAL_INT(XC_ERR_ZIP_UNSUPPORTED, open_image(&m));
    }
    ent e[] = {{"a.iso", XC_ZIP_DEFLATE, 0x0008U | 0x0002U | 0x0800U, "x", false, 0, 0, 0}};
    arc a = {false, NULL, 0, 0, 0};
    build(&a, e, 1);
    TEST_ASSERT_EQUAL_INT(XC_OK, open_image(&m)); /* data descriptor, level, UTF-8: fine */
    TEST_ASSERT_EQUAL_STRING("Deflate64", xc_zip_method_name(9));
    TEST_ASSERT_EQUAL_STRING("LZMA", xc_zip_method_name(14));
    TEST_ASSERT_EQUAL_STRING("stored", xc_zip_method_name(0));
    TEST_ASSERT_NULL(xc_zip_method_name(77));
}

static void test_bad_local_header(void)
{
    ent e[] = {{"game.iso", XC_ZIP_STORED, 0, "data", false, 0, 0, 0}};
    arc a = {false, NULL, 0, 0, 0};
    xc_zip_member m = {0, 0, 0, 0, 0, 0};
    build(&a, e, 1);
    set32(e[0].local_at, 0x04034b51U);
    TEST_ASSERT_EQUAL_INT(XC_ERR_CORRUPT, open_image(&m));
    build(&a, e, 1);
    set16(e[0].local_at + 8, XC_ZIP_DEFLATE); /* method differs */
    TEST_ASSERT_EQUAL_INT(XC_ERR_CORRUPT, open_image(&m));
    build(&a, e, 1);
    z[e[0].local_at + 30] = 'G'; /* name differs */
    TEST_ASSERT_EQUAL_INT(XC_ERR_CORRUPT, open_image(&m));
    build(&a, e, 1);
    set16(e[0].local_at + 26, 9); /* name length differs */
    TEST_ASSERT_EQUAL_INT(XC_ERR_CORRUPT, open_image(&m));
    build(&a, e, 1);
    set16(e[0].local_at + 28, 1); /* extra pushes the data into the CD */
    TEST_ASSERT_EQUAL_INT(XC_ERR_CORRUPT, open_image(&m));
    build(&a, e, 1);
    set32(e[0].local_at + 18, 0);           /* local sizes aren't used */
    set32(e[0].local_at + 14, 0xDEADBEEFU); /* nor its CRC */
    TEST_ASSERT_EQUAL_INT(XC_OK, open_image(&m));
    TEST_ASSERT_EQUAL_UINT64(4, m.compressed_size);
}

static void test_stored_sizes_must_match(void)
{
    ent e[] = {{"game.iso", XC_ZIP_STORED, 0, "data", false, 0, 0, 0}};
    arc a = {false, NULL, 0, 0, 0};
    xc_zip_member m = {0, 0, 0, 0, 0, 0};
    build(&a, e, 1);
    set32(e[0].central_at + 24, 5);
    TEST_ASSERT_EQUAL_INT(XC_ERR_CORRUPT, open_image(&m));
    build(&a, e, 1);
    set32(e[0].central_at + 20, 100); /* compressed data past the CD */
    set32(e[0].central_at + 24, 100);
    TEST_ASSERT_EQUAL_INT(XC_ERR_CORRUPT, open_image(&m));
}

static void test_invalid_arguments(void)
{
    xc_zip_listing l;
    xc_zip_member m = {0, 0, 0, 0, 0, 0};
    size_t i = 0;
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_zip_list(NULL, &l));
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_zip_list(&src, NULL));
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_zip_pick_image(NULL, &i));
    memset(&l, 0, sizeof l);
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_zip_open_member(&src, &l, 0, &m));
}

/* --- generated zips of the fixtures (tools/fixtures/zipgen.py) ------------------- */

static const char *zips_dir;

/* Reads a whole file into memory (the test zips are small). */
static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    TEST_ASSERT_EQUAL_INT(0, fseek(f, 0, SEEK_END));
    long n = ftell(f);
    TEST_ASSERT_TRUE(n >= 0);
    TEST_ASSERT_EQUAL_INT(0, fseek(f, 0, SEEK_SET));
    uint8_t *buf = malloc(n > 0 ? (size_t)n : 1U);
    TEST_ASSERT_NOT_NULL(buf);
    TEST_ASSERT_EQUAL_size_t((size_t)n, fread(buf, 1, (size_t)n, f));
    TEST_ASSERT_EQUAL_INT(0, fclose(f));
    *len = (size_t)n;
    return buf;
}

static void check_generated(const char *zip_name, uint16_t method, const char *iso_name)
{
    char path[1024];
    size_t zip_len = 0;
    size_t iso_len = 0;
    (void)snprintf(path, sizeof path, "%s/%s", zips_dir, zip_name);
    uint8_t *zip = slurp(path, &zip_len);
    (void)snprintf(path, sizeof path, "%s/%s", zips_dir, iso_name);
    uint8_t *iso = slurp(path, &iso_len);
    xc_mem_source zm;
    xc_source zs;
    xc_source_memory(&zs, &zm, zip, zip_len);
    xc_zip_listing l;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_zip_list(&zs, &l));
    size_t i = 0;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_zip_pick_image(&l, &i));
    xc_zip_member m = {0, 0, 0, 0, 0, 0};
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_zip_open_member(&zs, &l, i, &m));
    TEST_ASSERT_EQUAL_UINT16(method, m.method);
    TEST_ASSERT_EQUAL_UINT64(iso_len, m.size);
    TEST_ASSERT_EQUAL_UINT32(xc_crc32(0, iso, iso_len), m.crc32);
    if (method == XC_ZIP_STORED) {
        TEST_ASSERT_EQUAL_MEMORY(iso, zip + m.data_offset, iso_len);
    }
    xc_zip_listing_free(&l);
    free(zip);
    free(iso);
}

static void test_generated_stored(void)
{
    check_generated("small-stored.zip", XC_ZIP_STORED, "small.iso");
}

static void test_generated_deflate(void)
{
    check_generated("small-deflate.zip", XC_ZIP_DEFLATE, "small.iso");
}

int main(int argc, char **argv)
{
    UNITY_BEGIN();
    RUN_TEST(test_one_stored_member);
    RUN_TEST(test_zip64_records_and_extra);
    RUN_TEST(test_comment_with_fake_signature);
    RUN_TEST(test_longest_comment);
    RUN_TEST(test_empty_zip);
    RUN_TEST(test_not_zip);
    RUN_TEST(test_split_archive_unsupported);
    RUN_TEST(test_bad_zip64_end_is_corrupt);
    RUN_TEST(test_central_directory_bounds);
    RUN_TEST(test_limits);
    RUN_TEST(test_extra_fields);
    RUN_TEST(test_disk_number_unsupported);
    RUN_TEST(test_image_names);
    RUN_TEST(test_pick_none_or_several);
    RUN_TEST(test_unsupported_members);
    RUN_TEST(test_bad_local_header);
    RUN_TEST(test_stored_sizes_must_match);
    RUN_TEST(test_invalid_arguments);
    if (argc > 1) {
        zips_dir = argv[1];
        RUN_TEST(test_generated_stored);
        RUN_TEST(test_generated_deflate);
    }
    return UNITY_END();
}
