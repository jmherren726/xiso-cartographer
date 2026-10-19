/* Tests for the FATX rules (xcat/fatx.h, docs/formats/fatx-names.md, ADR 0008). */
#include "unity.h"
#include "xcat/fatx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* --- §2: the byte table equals the probe's measured list ----------------------- */

/* Written out independently of fatx.c's bitmap: app/fatx-probe v2, kernel 1.0.4627.1. */
static const uint8_t measured_rejected_high[] = {
    0x84, 0x86, 0x87, 0x89, 0x8B, 0x8F, 0x91, 0x9F, 0xA0, 0xA1, 0xA2, 0xA5, 0xA6, 0xA9,
    0xAA, 0xAD, 0xAE, 0xB0, 0xBF, 0xC0, 0xC1, 0xC2, 0xC5, 0xC6, 0xC9, 0xCA, 0xCD, 0xCE,
    0xD0, 0xDF, 0xE0, 0xE1, 0xE4, 0xE5, 0xE6, 0xE7, 0xE9, 0xEF, 0xF1, 0xFF};
static const char measured_rejected_ascii[] = "\"*+,/:;<=>?\\|";

static bool expected_ok(unsigned c)
{
    if (c < 0x20) {
        return false; /* control bytes, and 0x00 */
    }
    if (c < 0x80) {
        return strchr(measured_rejected_ascii, (int)c) == NULL;
    }
    for (size_t i = 0; i < sizeof measured_rejected_high; i++) {
        if (measured_rejected_high[i] == c) {
            return false;
        }
    }
    return true;
}

static void test_byte_table_matches_probe(void)
{
    unsigned accepted = 0;
    for (unsigned c = 0; c < 256; c++) {
        char msg[32];
        (void)snprintf(msg, sizeof msg, "byte 0x%02X", c);
        TEST_ASSERT_EQUAL_MESSAGE(expected_ok(c), xc_fatx_byte_ok((uint8_t)c), msg);
        accepted += xc_fatx_byte_ok((uint8_t)c) ? 1U : 0U;
    }
    TEST_ASSERT_EQUAL_UINT(171, accepted); /* as the probe reported */
}

/* --- §1: names -------------------------------------------------------------------- */

static void test_name_checks(void)
{
    size_t bad = 99;
    TEST_ASSERT_EQUAL_INT(XC_FATX_NAME_OK, xc_fatx_check_name("default.xbe", 11, NULL));
    TEST_ASSERT_EQUAL_INT(XC_FATX_NAME_OK, xc_fatx_check_name("trailing. ", 10, NULL));
    TEST_ASSERT_EQUAL_INT(XC_FATX_NAME_EMPTY, xc_fatx_check_name("", 0, NULL));
    TEST_ASSERT_EQUAL_INT(XC_FATX_NAME_DOTS, xc_fatx_check_name(".", 1, NULL));
    TEST_ASSERT_EQUAL_INT(XC_FATX_NAME_DOTS, xc_fatx_check_name("..", 2, NULL));
    TEST_ASSERT_EQUAL_INT(XC_FATX_NAME_OK, xc_fatx_check_name("...", 3, NULL));
    char name[44];
    memset(name, 'a', sizeof name);
    TEST_ASSERT_EQUAL_INT(XC_FATX_NAME_OK, xc_fatx_check_name(name, 42, NULL));
    TEST_ASSERT_EQUAL_INT(XC_FATX_NAME_TOO_LONG, xc_fatx_check_name(name, 43, NULL));
    TEST_ASSERT_EQUAL_INT(XC_FATX_NAME_BAD_BYTE, xc_fatx_check_name("ab?c", 4, &bad));
    TEST_ASSERT_EQUAL_size_t(2, bad);
}

/* --- §3, §4: listings ------------------------------------------------------------- */

static xc_xdvdfs_entry entry(const char *path, uint8_t attributes)
{
    xc_xdvdfs_entry e = {path, 0, 0, 0, attributes};
    return e;
}

static void test_valid_listing_has_no_problems(void)
{
    xc_xdvdfs_entry dirs[] = {entry("media", 0x10)};
    xc_xdvdfs_entry files[] = {entry("default.xbe", 0x20), entry("media/intro.xmv", 0x20)};
    xc_xdvdfs_listing l = {files, 2, dirs, 1, 0, NULL, NULL, 0, false};
    size_t count = 99;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_fatx_check_listing(&l, 11, NULL, 0, &count));
    TEST_ASSERT_EQUAL_size_t(0, count);
}

/* §4b: measured on hardware (probe v4): 4 GiB - 16 KiB grows fine, one byte more doesn't. */
static void test_file_size_limit(void)
{
    xc_xdvdfs_entry files[] = {entry("max.bin", 0x20), entry("over.bin", 0x20),
                               entry("way-over.bin", 0x20)};
    files[0].size = 4294950912U; /* 4 GiB - 16 KiB */
    files[1].size = 4294950913U;
    files[2].size = 0xFFFFFFFFU;
    xc_xdvdfs_entry dirs[] = {entry("d", 0x10)};
    dirs[0].size = 0xFFFFFFFFU; /* a directory's size is its table: never checked here */
    xc_xdvdfs_listing l = {files, 3, dirs, 1, 0, NULL, NULL, 0, false};
    xc_fatx_problem p[4];
    size_t count = 0;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_fatx_check_listing(&l, 11, p, 4, &count));
    TEST_ASSERT_EQUAL_size_t(2, count);
    TEST_ASSERT_EQUAL_INT(XC_FATX_PROBLEM_FILE_TOO_BIG, p[0].kind);
    TEST_ASSERT_EQUAL_STRING("over.bin", p[0].path);
    TEST_ASSERT_EQUAL_size_t(4294950913U, p[0].value);
    TEST_ASSERT_EQUAL_STRING("way-over.bin", p[1].path);
}

static void test_bad_names_are_reported(void)
{
    char long_name[44];
    memset(long_name, 'n', 43);
    long_name[43] = '\0';
    xc_xdvdfs_entry files[] = {entry("ok.bin", 0x20), entry("what?.bin", 0x20),
                               entry(long_name, 0x20)};
    xc_xdvdfs_listing l = {files, 3, NULL, 0, 0, NULL, NULL, 0, false};
    xc_fatx_problem p[4];
    size_t count = 0;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_fatx_check_listing(&l, 11, p, 4, &count));
    TEST_ASSERT_EQUAL_size_t(2, count);
    TEST_ASSERT_EQUAL_INT(XC_FATX_PROBLEM_NAME, p[0].kind);
    TEST_ASSERT_EQUAL_INT(XC_FATX_NAME_BAD_BYTE, p[0].name);
    TEST_ASSERT_EQUAL_STRING("what?.bin", p[0].path);
    TEST_ASSERT_EQUAL_size_t(4, p[0].value);
    TEST_ASSERT_EQUAL_INT(XC_FATX_NAME_TOO_LONG, p[1].name);
}

static void test_path_limit_is_exactly_250(void)
{
    /* install dir "\Games\Title" = 12 bytes; "\" + path must keep the total <= 250 */
    static char at[240];
    static char over[240];
    memset(at, 'a', sizeof at);
    memset(over, 'a', sizeof over);
    for (size_t i = 40; i < sizeof at; i += 41) {
        at[i] = '/';
        over[i] = '/';
    }
    at[237] = '\0'; /* 12 + 1 + 237 = 250 */
    over[238] = '\0';
    xc_xdvdfs_entry files[] = {entry(at, 0x20), entry(over, 0x20)};
    xc_xdvdfs_listing l = {files, 2, NULL, 0, 0, NULL, NULL, 0, false};
    xc_fatx_problem p[2];
    size_t count = 0;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_fatx_check_listing(&l, 12, p, 2, &count));
    TEST_ASSERT_EQUAL_size_t(1, count);
    TEST_ASSERT_EQUAL_INT(XC_FATX_PROBLEM_PATH_TOO_LONG, p[0].kind);
    TEST_ASSERT_EQUAL_PTR(over, p[0].path);
    TEST_ASSERT_EQUAL_size_t(251, p[0].value);
}

static void test_directory_entry_limit(void)
{
    enum { N = 4097 + 4096 }; /* 4,097 files in "big" (too many); the rest at the root */
    xc_xdvdfs_entry *files = calloc(N, sizeof *files);
    char (*names)[16] = calloc(N, sizeof *names);
    TEST_ASSERT_NOT_NULL(files);
    TEST_ASSERT_NOT_NULL(names);
    for (size_t i = 0; i < N; i++) {
        if (i < 4097) {
            (void)snprintf(names[i], sizeof names[i], "big/f%05u", (unsigned)i);
        } else {
            (void)snprintf(names[i], sizeof names[i], "r%05u", (unsigned)i);
        }
        files[i] = entry(names[i], 0x20);
    }
    /* Use N - 1 files: the root then holds 4,095 files plus the directory "big" = 4,096. */
    xc_xdvdfs_entry dirs[] = {entry("big", 0x10)};
    xc_xdvdfs_listing l = {files, N - 1, dirs, 1, 0, NULL, NULL, 0, false};
    xc_fatx_problem p[2];
    size_t count = 0;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_fatx_check_listing(&l, 12, p, 2, &count));
    TEST_ASSERT_EQUAL_size_t(1, count);
    TEST_ASSERT_EQUAL_INT(XC_FATX_PROBLEM_TOO_MANY_ENTRIES, p[0].kind);
    TEST_ASSERT_EQUAL_STRING("big", p[0].path);
    TEST_ASSERT_EQUAL_size_t(4097, p[0].value);
    free(names);
    free(files);
}

static void test_problem_count_exceeds_storage(void)
{
    xc_xdvdfs_entry files[] = {entry("a?", 0x20), entry("b?", 0x20), entry("c?", 0x20)};
    xc_xdvdfs_listing l = {files, 3, NULL, 0, 0, NULL, NULL, 0, false};
    xc_fatx_problem p[1];
    size_t count = 0;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_fatx_check_listing(&l, 12, p, 1, &count));
    TEST_ASSERT_EQUAL_size_t(3, count); /* all counted, one stored */
    TEST_ASSERT_EQUAL_STRING("a?", p[0].path);
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_fatx_check_listing(&l, 251, p, 1, &count));
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_fatx_check_listing(NULL, 12, p, 1, &count));
}

/* --- §5: folder names ------------------------------------------------------------- */

static void expect_title(const char *title, size_t max_len, const char *expected)
{
    char out[XC_FATX_MAX_NAME + 1];
    size_t n = xc_fatx_title_name(title, out, max_len);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(expected, out, title != NULL ? title : "(null)");
    TEST_ASSERT_EQUAL_size_t(strlen(expected), n);
    TEST_ASSERT_EQUAL_INT(XC_FATX_NAME_OK, xc_fatx_check_name(out, n, NULL));
}

static void test_title_names(void)
{
    expect_title("Halo: Combat Evolved", 42, "Halo - Combat Evolved");
    expect_title("Tom Clancy's Splinter Cell (USA)", 42, "Tom Clancy's Splinter Cell (USA)");
    expect_title("a/b*c?d", 42, "a_b_c_d");
    expect_title("  ..Spaced Out..  ", 42, "Spaced Out");
    expect_title("Caf\xc3\xa9 Racer", 42, "Caf_ Racer"); /* one '_' per invalid run */
    expect_title("", 42, "Game");
    expect_title(NULL, 42, "Game");
    expect_title("???", 42, "Game");
    expect_title("Halo: Combat Evolved", 5, "Halo");
    expect_title("Star Wars Knights of the Old Republic (USA) (Rev 1)", 42,
                 "Star Wars Knights of the Old Republic (USA");
    expect_title("Anything", 0, "A"); /* max_len clamped to at least 1 */
}

static void test_random_titles_always_valid(void)
{
    uint32_t x = 12345;
    for (int round = 0; round < 20000; round++) {
        char title[64];
        size_t len = 0;
        for (; len < sizeof title - 1; len++) {
            x = x * 1664525U + 1013904223U;
            uint8_t c = (uint8_t)(x >> 24U);
            if (c == 0 || (x & 0x3F00U) == 0) {
                break;
            }
            title[len] = (char)c;
        }
        title[len] = '\0';
        char out[XC_FATX_MAX_NAME + 1];
        size_t max_len = 1 + (x >> 8U) % XC_FATX_MAX_NAME;
        size_t n = xc_fatx_title_name(title, out, max_len);
        TEST_ASSERT_TRUE(n >= 1 && n <= max_len);
        TEST_ASSERT_EQUAL_size_t(n, strlen(out));
        TEST_ASSERT_EQUAL_INT(XC_FATX_NAME_OK, xc_fatx_check_name(out, n, NULL));
    }
}

static void test_numbered_names(void)
{
    char out[XC_FATX_MAX_NAME + 1];
    TEST_ASSERT_EQUAL_size_t(8, xc_fatx_numbered_name("Halo", 2, out));
    TEST_ASSERT_EQUAL_STRING("Halo (2)", out);
    char base[XC_FATX_MAX_NAME + 1];
    memset(base, 'x', XC_FATX_MAX_NAME);
    base[XC_FATX_MAX_NAME] = '\0';
    base[37] = ' '; /* truncation would leave a trailing space */
    size_t n = xc_fatx_numbered_name(base, 12, out);
    TEST_ASSERT_TRUE(n <= XC_FATX_MAX_NAME);
    TEST_ASSERT_EQUAL_STRING("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx (12)", out);
    TEST_ASSERT_EQUAL_INT(XC_FATX_NAME_OK, xc_fatx_check_name(out, n, NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_byte_table_matches_probe);
    RUN_TEST(test_name_checks);
    RUN_TEST(test_valid_listing_has_no_problems);
    RUN_TEST(test_bad_names_are_reported);
    RUN_TEST(test_path_limit_is_exactly_250);
    RUN_TEST(test_directory_entry_limit);
    RUN_TEST(test_problem_count_exceeds_storage);
    RUN_TEST(test_title_names);
    RUN_TEST(test_random_titles_always_valid);
    RUN_TEST(test_numbered_names);
    RUN_TEST(test_file_size_limit);
    return UNITY_END();
}
