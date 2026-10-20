/* Tests for archive.org item listings (xcat/archiveorg.h, docs/formats/archiveorg.md). */
#include "unity.h"
#include "xcat/archiveorg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static xc_archiveorg_item item;

static xc_err parse(const char *body)
{
    return xc_archiveorg_parse("test-item_1.0", body, strlen(body), &item);
}

static const char listing[] =
    "{\"created\":1,\"metadata\":{\"title\":\"x\",\"description\":\"a \\\"long\\\" text\"},"
    "\"files\":["
    "{\"name\":\"Game A (USA).zip\",\"source\":\"original\",\"size\":\"6266337551\","
    "\"md5\":\"38b167e84cd7f5342419b83e7896c992\",\"crc32\":\"57654766\","
    "\"sha1\":\"0BF206772BB82EAD1EB27C24531C2042B719011B\",\"format\":\"ZIP\",\"private\":\"true\"}"
    ","
    "{\"name\":\"Game A (USA).jpg\",\"source\":\"original\",\"size\":\"1000\"},"
    "{\"name\":\"thumb.zip\",\"source\":\"derivative\",\"size\":\"10\"},"
    "{\"name\":\"item_meta.xml\",\"source\":\"original\",\"size\":\"10\"},"
    "{\"name\":\"disc/Game B #1 (Europe).xiso.ISO\",\"size\":3728867328,\"md5\":\"zz\"},"
    "{\"name\":\"Game C.xiso\",\"source\":\"original\",\"size\":\"1\",\"crc32\":7}"
    "],\"files_count\":6,\"is_dark\":false}";

static void test_listing(void)
{
    TEST_ASSERT_EQUAL_INT(XC_OK, parse(listing));
    TEST_ASSERT_EQUAL_INT(XC_ARCHIVEORG_OK, item.status);
    TEST_ASSERT_EQUAL_size_t(3, item.file_count);
    TEST_ASSERT_EQUAL_size_t(0, item.invalid);
    TEST_ASSERT_EQUAL_size_t(2, item.hashes_dropped);
    TEST_ASSERT_FALSE(item.truncated);

    const xc_archiveorg_file *a = &item.files[0];
    TEST_ASSERT_EQUAL_STRING("Game A (USA).zip", a->path);
    TEST_ASSERT_EQUAL_STRING("Game A (USA)", a->name);
    TEST_ASSERT_EQUAL_STRING("https://archive.org/download/test-item_1.0/Game%20A%20%28USA%29.zip",
                             a->url);
    TEST_ASSERT_TRUE(a->size == UINT64_C(6266337551));
    TEST_ASSERT_TRUE(a->zip);
    TEST_ASSERT_TRUE(a->private_file);
    TEST_ASSERT_TRUE(a->has_md5 && a->has_sha1 && a->has_crc32);
    TEST_ASSERT_EQUAL_HEX8(0x38, a->md5[0]);
    TEST_ASSERT_EQUAL_HEX8(0x92, a->md5[15]);
    TEST_ASSERT_EQUAL_HEX8(0x0B, a->sha1[0]);
    TEST_ASSERT_EQUAL_HEX8(0x1B, a->sha1[19]);
    TEST_ASSERT_EQUAL_HEX32(0x57654766, a->crc32);

    const xc_archiveorg_file *b = &item.files[1];
    TEST_ASSERT_EQUAL_STRING("disc/Game B #1 (Europe).xiso.ISO", b->path);
    TEST_ASSERT_EQUAL_STRING("Game B #1 (Europe)", b->name);
    TEST_ASSERT_EQUAL_STRING(
        "https://archive.org/download/test-item_1.0/disc/Game%20B%20%231%20%28Europe%29.xiso.ISO",
        b->url);
    TEST_ASSERT_TRUE(b->size == UINT64_C(3728867328));
    TEST_ASSERT_FALSE(b->zip);
    TEST_ASSERT_FALSE(b->private_file);
    TEST_ASSERT_FALSE(b->has_md5 || b->has_sha1 || b->has_crc32);

    const xc_archiveorg_file *c = &item.files[2];
    TEST_ASSERT_EQUAL_STRING("Game C", c->name);
    TEST_ASSERT_TRUE(c->size == 1);
    TEST_ASSERT_FALSE(c->has_crc32); /* a number, not a string */
    xc_archiveorg_free(&item);
    TEST_ASSERT_NULL(item.files);
}

static void test_status(void)
{
    static const struct {
        const char *body;
        xc_archiveorg_status status;
    } cases[] = {
        {"{}", XC_ARCHIVEORG_MISSING},
        {"{\"error\":\"no such item\"}", XC_ARCHIVEORG_MISSING},
        {"{\"is_dark\":true}", XC_ARCHIVEORG_DARK},
        {"{\"files\":[{\"name\":\"a.iso\",\"size\":\"5\"}],\"is_dark\":true}", XC_ARCHIVEORG_DARK},
        {"{\"is_dark\":\"true\",\"files\":[]}", XC_ARCHIVEORG_OK},
        {"{\"files\":[]}", XC_ARCHIVEORG_OK},
        {"{\"files\":[1,\"x\",null,[],{}]}", XC_ARCHIVEORG_OK},
        {"", XC_ARCHIVEORG_NOT_METADATA},
        {"[]", XC_ARCHIVEORG_NOT_METADATA},
        {"<html>busy</html>", XC_ARCHIVEORG_NOT_METADATA},
        {"{\"files\":{}}", XC_ARCHIVEORG_NOT_METADATA},
        {"{\"files\":[],\"files\":[]}", XC_ARCHIVEORG_NOT_METADATA},
        {"{\"files\":[{\"name\":\"a.iso\",\"size\":\"5\"}]", XC_ARCHIVEORG_NOT_METADATA},
        {"{\"files\":[{\"name\":\"a.iso\",\"size\":\"5\"}]} x", XC_ARCHIVEORG_NOT_METADATA},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(XC_OK, parse(cases[i].body), cases[i].body);
        TEST_ASSERT_EQUAL_INT_MESSAGE(cases[i].status, item.status, cases[i].body);
        if (item.status != XC_ARCHIVEORG_OK) {
            TEST_ASSERT_EQUAL_size_t_MESSAGE(0, item.file_count, cases[i].body);
        }
        xc_archiveorg_free(&item);
    }
}

/* Parses a listing with one file named `name` (raw JSON text) and size `size`. */
static void one(const char *name, const char *size)
{
    char body[2048];
    (void)snprintf(body, sizeof body, "{\"files\":[{\"name\":%s,\"size\":%s}]}", name, size);
    TEST_ASSERT_EQUAL_INT(XC_OK, parse(body));
    TEST_ASSERT_EQUAL_INT_MESSAGE(XC_ARCHIVEORG_OK, item.status, body);
}

static void test_invalid_files(void)
{
    static const char *const bad_names[] = {
        "\"/a.iso\"",    "\"a//b.iso\"",      "\"./a.iso\"", "\"a/../b.iso\"", "\"a\\\\b.iso\"",
        "\"a\\tb.iso\"", "\"a\\u007fb.iso\"", "\".zip\"",    "\"dir/.iso\"",   "\"a/./b.iso\"",
    };
    for (size_t i = 0; i < sizeof bad_names / sizeof bad_names[0]; i++) {
        char body[256];
        (void)snprintf(body, sizeof body, "{\"files\":[{\"name\":%s,\"size\":\"9\"}]}",
                       bad_names[i]);
        TEST_ASSERT_EQUAL_INT(XC_OK, parse(body));
        if (item.status == XC_ARCHIVEORG_OK) {
            TEST_ASSERT_EQUAL_size_t_MESSAGE(0, item.file_count, bad_names[i]);
            TEST_ASSERT_EQUAL_size_t_MESSAGE(1, item.invalid, bad_names[i]);
            TEST_ASSERT_TRUE(strchr(item.first_invalid, '\t') == NULL);
        }
        xc_archiveorg_free(&item);
    }

    static const char *const bad_sizes[] = {
        "\"0\"",      "0",    "\"012\"", "\"-1\"", "-1",     "1.5",
        "1e3",        "\"\"", "null",    "true",   "\"1 \"", "\"9223372036854775808\"",
        "\"\\u0031\""};
    for (size_t i = 0; i < sizeof bad_sizes / sizeof bad_sizes[0]; i++) {
        one("\"x.iso\"", bad_sizes[i]);
        TEST_ASSERT_EQUAL_size_t_MESSAGE(0, item.file_count, bad_sizes[i]);
        TEST_ASSERT_EQUAL_size_t_MESSAGE(1, item.invalid, bad_sizes[i]);
        TEST_ASSERT_EQUAL_STRING("x.iso", item.first_invalid);
        xc_archiveorg_free(&item);
    }
    one("\"x.iso\"", "\"9223372036854775807\"");
    TEST_ASSERT_EQUAL_size_t(1, item.file_count);
    TEST_ASSERT_TRUE(item.files[0].size == UINT64_C(9223372036854775807));
    xc_archiveorg_free(&item);

    /* No size at all. */
    TEST_ASSERT_EQUAL_INT(XC_OK, parse("{\"files\":[{\"name\":\"x.zip\"}]}"));
    TEST_ASSERT_EQUAL_size_t(1, item.invalid);
    xc_archiveorg_free(&item);

    /* Names that aren't image files, or aren't strings, are skipped silently. */
    static const char *const silent[] = {"\"x.isox\"", "\"x.7z\"", "5", "null", "\"\\ud800.iso\""};
    for (size_t i = 0; i < sizeof silent / sizeof silent[0]; i++) {
        one(silent[i], "\"9\"");
        TEST_ASSERT_EQUAL_size_t_MESSAGE(0, item.file_count, silent[i]);
        TEST_ASSERT_EQUAL_size_t_MESSAGE(0, item.invalid, silent[i]);
        xc_archiveorg_free(&item);
    }
}

static void test_names(void)
{
    /* The longest matching extension, any case; non-ASCII is percent-encoded byte by byte. */
    one("\"A.XISO.ISO\"", "\"9\"");
    TEST_ASSERT_EQUAL_STRING("A", item.files[0].name);
    xc_archiveorg_free(&item);
    one("\"a.iso.zip\"", "\"9\"");
    TEST_ASSERT_EQUAL_STRING("a.iso", item.files[0].name);
    TEST_ASSERT_TRUE(item.files[0].zip);
    xc_archiveorg_free(&item);
    one("\"Caf\\u00e9 ~x+y.iso\"", "\"9\"");
    TEST_ASSERT_EQUAL_STRING("Caf\xc3\xa9 ~x+y", item.files[0].name);
    TEST_ASSERT_EQUAL_STRING("https://archive.org/download/test-item_1.0/Caf%C3%A9%20~x%2By.iso",
                             item.files[0].url);
    xc_archiveorg_free(&item);

    /* 512 bytes is the longest name. */
    char name[600];
    for (size_t len = 512; len <= 513; len++) {
        name[0] = '"';
        memset(name + 1, 'n', len - 4);
        memcpy(name + 1 + len - 4, ".iso\"", 6);
        one(name, "\"9\"");
        TEST_ASSERT_EQUAL_size_t(len == 512 ? 1U : 0U, item.file_count);
        TEST_ASSERT_EQUAL_size_t(len == 512 ? 0U : 1U, item.invalid);
        xc_archiveorg_free(&item);
    }
}

static void test_ids(void)
{
    char url[160];
    TEST_ASSERT_TRUE(xc_archiveorg_valid_id("a"));
    TEST_ASSERT_FALSE(xc_archiveorg_valid_id(""));
    TEST_ASSERT_FALSE(xc_archiveorg_valid_id("a/b"));
    TEST_ASSERT_FALSE(xc_archiveorg_valid_id("a b"));
    TEST_ASSERT_FALSE(xc_archiveorg_valid_id("a%2f"));
    char id[XC_ARCHIVEORG_MAX_ID + 2];
    memset(id, 'x', sizeof id - 1);
    id[sizeof id - 1] = '\0';
    TEST_ASSERT_FALSE(xc_archiveorg_valid_id(id));
    id[XC_ARCHIVEORG_MAX_ID] = '\0';
    TEST_ASSERT_TRUE(xc_archiveorg_valid_id(id));

    TEST_ASSERT_EQUAL_INT(XC_OK, xc_archiveorg_metadata_url("my_item-2", url, sizeof url));
    TEST_ASSERT_EQUAL_STRING("https://archive.org/metadata/my_item-2", url);
    TEST_ASSERT_EQUAL_INT(XC_ERR_LIMIT, xc_archiveorg_metadata_url("my_item-2", url, 20));
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_archiveorg_metadata_url("a?b", url, sizeof url));
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_archiveorg_parse("a?b", "{}", 2, &item));
    xc_archiveorg_free(&item);
}

static void test_limits(void)
{
    size_t len = XC_ARCHIVEORG_MAX_METADATA + 1;
    char *big = malloc(len);
    TEST_ASSERT_NOT_NULL(big);
    memset(big, ' ', len);
    big[0] = '{';
    big[len - 1] = '}';
    TEST_ASSERT_EQUAL_INT(XC_ERR_LIMIT, xc_archiveorg_parse("a", big, len, &item));
    xc_archiveorg_free(&item);
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_archiveorg_parse("a", big, len - 1, &item));
    TEST_ASSERT_EQUAL_INT(XC_ARCHIVEORG_NOT_METADATA, item.status); /* lost its '}' */
    xc_archiveorg_free(&item);

    /* More files than a map may hold: the rest are dropped and flagged. */
    const char *file = "{\"name\":\"a.iso\",\"size\":\"1\"},";
    size_t n = XC_ARCHIVEORG_MAX_FILES + 2;
    size_t flen = strlen(file);
    char *body = malloc(16 + n * flen);
    TEST_ASSERT_NOT_NULL(body);
    size_t at = (size_t)sprintf(body, "{\"files\":[");
    for (size_t i = 0; i < n; i++) {
        memcpy(body + at, file, flen + 1);
        at += flen;
    }
    body[at - 1] = ']';
    body[at++] = '}';
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_archiveorg_parse("a", body, at, &item));
    TEST_ASSERT_EQUAL_INT(XC_ARCHIVEORG_OK, item.status);
    TEST_ASSERT_EQUAL_size_t(XC_ARCHIVEORG_MAX_FILES, item.file_count);
    TEST_ASSERT_TRUE(item.truncated);
    xc_archiveorg_free(&item);
    free(body);
    free(big);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_listing);
    RUN_TEST(test_status);
    RUN_TEST(test_invalid_files);
    RUN_TEST(test_names);
    RUN_TEST(test_ids);
    RUN_TEST(test_limits);
    return UNITY_END();
}
