/* Tests for the install journal (core/src/journal.c, docs/formats/journal.md). */
#include "unity.h"
#include "xcat/crc32.h"
#include "xcat/journal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static xc_xdvdfs_entry entries[] = {
    {"default.xbe", 0x10000, 12345, 32, 0},
    {"media/a b.bik", 0x20000, 4294965248U, 64, 0},
    {"media/100%.wav", 0x40000, 0, 0, 0},
    {"caf\xC3\xA9/-", 0x50000, 1, 160, 0},
};
static xc_xdvdfs_listing listing = {entries, 4, NULL, 0, 0, NULL};
static xc_journal_id image;
static xc_journal_state st;
static char buf[65536];
static size_t used;

void setUp(void)
{
    xc_journal_make_id(&image, &listing, 0x18300000, 7825162240U, "\"5e1f-61a2c3\"",
                       "Wed, 14 Oct 2026 10:00:00 GMT");
    memset(&st, 0, sizeof st);
    used = 0;
}

void tearDown(void)
{
    xc_journal_state_free(&st);
}

static void start(void)
{
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_journal_format_start(&image, "http://192.168.1.20:8080/x y.iso",
                                                         buf + used, sizeof buf - used, &n));
    used += n;
}

static void done(size_t i, uint32_t crc)
{
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_journal_format_done(entries[i].path, entries[i].size, crc,
                                                        buf + used, sizeof buf - used, &n));
    used += n;
}

static void part(size_t i, uint32_t bytes, uint32_t crc)
{
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_journal_format_part(entries[i].path, bytes, crc, buf + used,
                                                        sizeof buf - used, &n));
    used += n;
}

static void read_back(size_t len)
{
    xc_journal_state_free(&st);
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_journal_read(buf, len, &listing, &image, &st));
}

/* Appends one line with a correct CRC, for hand-made records. */
static void raw_line(const char *content)
{
    uint32_t crc = xc_crc32(0, content, strlen(content));
    used += (size_t)snprintf(buf + used, sizeof buf - used, "%s *%08X\n", content, (unsigned)crc);
}

static void expect_invalid(const char *why)
{
    read_back(used);
    TEST_ASSERT_FALSE(st.valid);
    TEST_ASSERT_NOT_NULL(strstr(st.why, why));
    TEST_ASSERT_EQUAL_size_t(0, st.done_count);
    for (size_t i = 0; i < listing.file_count; i++) {
        TEST_ASSERT_FALSE(st.done[i]);
    }
}

static void test_round_trip(void)
{
    start();
    done(1, 0x9A0C3B71U);
    done(3, 0x00000001U);
    done(2, 0U);
    read_back(used);
    TEST_ASSERT_TRUE(st.valid);
    TEST_ASSERT_FALSE(st.torn);
    TEST_ASSERT_EQUAL_size_t(3, st.done_count);
    TEST_ASSERT_FALSE(st.done[0]);
    TEST_ASSERT_TRUE(st.done[1]);
    TEST_ASSERT_EQUAL_HEX32(0x9A0C3B71U, st.crc[1]);
    TEST_ASSERT_TRUE(st.done[2]);
    TEST_ASSERT_TRUE(st.done[3]);
    TEST_ASSERT_EQUAL_HEX32(1U, st.crc[3]);
    TEST_ASSERT_EQUAL_STRING("\"5e1f-61a2c3\"", st.id.etag);
    TEST_ASSERT_EQUAL_STRING("Wed, 14 Oct 2026 10:00:00 GMT", st.id.last_modified);
    TEST_ASSERT_EQUAL_UINT64(7825162240U, st.id.image_size);
    TEST_ASSERT_EQUAL_UINT64(0x18300000U, st.id.partition_offset);
    /* The lines look as journal.md §1 shows them. */
    buf[used] = '\0';
    TEST_ASSERT_EQUAL_INT(0, strncmp(buf, "# xcat-journal v1.1 *", 21));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\nimage 7825162240 405798912 "));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\nvalidators \"5e1f-61a2c3\" Wed,%2014%20Oct%202026"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\nsource http://192.168.1.20:8080/x%20y.iso *"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\ndone 4294965248 9A0C3B71 media/a%20b.bik *"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\ndone 1 00000001 caf%C3%A9/- *"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\ndone 0 00000000 media/100%25.wav *"));
    /* Only the identity: valid, nothing done yet. CRLF is tolerated. */
    used = 0;
    start();
    char crlf[4096];
    size_t n = 0;
    for (size_t i = 0; i < used; i++) {
        if (buf[i] == '\n') {
            crlf[n++] = '\r';
        }
        crlf[n++] = buf[i];
    }
    xc_journal_state_free(&st);
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_journal_read(crlf, n, &listing, &image, &st));
    TEST_ASSERT_TRUE(st.valid);
    TEST_ASSERT_EQUAL_size_t(0, st.done_count);
}

static void test_torn_tail_at_every_byte(void)
{
    start();
    size_t identity = used;
    done(0, 0x11111111U);
    size_t one = used;
    done(1, 0x22222222U);
    for (size_t cut = identity; cut <= used; cut++) {
        read_back(cut);
        TEST_ASSERT_TRUE(st.valid);
        size_t want = cut == used ? 2U : cut >= one ? 1U : 0U;
        TEST_ASSERT_EQUAL_size_t(want, st.done_count);
        TEST_ASSERT_EQUAL(cut != used && cut != one && cut != identity, st.torn);
    }
    /* A cut inside the identity lines makes the journal unusable. */
    for (size_t cut = 0; cut < identity; cut++) {
        read_back(cut);
        TEST_ASSERT_FALSE(st.valid);
    }
}

static void test_damaged_line_stops_reading(void)
{
    start();
    done(0, 1U);
    size_t second = used;
    done(1, 2U);
    done(2, 3U);
    buf[second + 6] =
        buf[second + 6] == '2' ? '3' : '2'; /* change one digit of the second done line */
    read_back(used);
    TEST_ASSERT_TRUE(st.valid);
    TEST_ASSERT_TRUE(st.torn);
    TEST_ASSERT_EQUAL_size_t(1, st.done_count); /* the third line is ignored too (§6.1) */
    TEST_ASSERT_TRUE(st.done[0]);
}

static void test_other_image(void)
{
    start();
    done(0, 1U);
    xc_journal_id other = image;
    other.image_size++;
    xc_journal_state_free(&st);
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_journal_read(buf, used, &listing, &other, &st));
    TEST_ASSERT_FALSE(st.valid);
    TEST_ASSERT_NOT_NULL(strstr(st.why, "different image"));
    TEST_ASSERT_EQUAL_size_t(0, st.done_count);
    /* Validators count only when both sides have one. */
    xc_journal_id a = image;
    xc_journal_id b = image;
    b.etag[0] = '\0';
    TEST_ASSERT_TRUE(xc_journal_same_image(&a, &b));
    memcpy(b.etag, "\"other\"", 8);
    TEST_ASSERT_FALSE(xc_journal_same_image(&a, &b));
    b = image;
    b.last_modified[0] = 'X';
    TEST_ASSERT_FALSE(xc_journal_same_image(&a, &b));
    b = image;
    b.partition_offset = 0;
    TEST_ASSERT_FALSE(xc_journal_same_image(&a, &b));
    b = image;
    b.file_count++;
    TEST_ASSERT_FALSE(xc_journal_same_image(&a, &b));
}

static void test_listing_crc(void)
{
    uint32_t base = xc_journal_listing_crc(&listing);
    entries[1].offset++;
    TEST_ASSERT_NOT_EQUAL(base, xc_journal_listing_crc(&listing));
    entries[1].offset--;
    entries[2].size++;
    TEST_ASSERT_NOT_EQUAL(base, xc_journal_listing_crc(&listing));
    entries[2].size--;
    entries[0].path = "default.xbf";
    TEST_ASSERT_NOT_EQUAL(base, xc_journal_listing_crc(&listing));
    entries[0].path = "default.xbe";
    TEST_ASSERT_EQUAL_HEX32(base, xc_journal_listing_crc(&listing));
    /* "<offset> <size> <path>\n" per file, in listing order (§4). */
    char text[512];
    int n = 0;
    for (size_t i = 0; i < listing.file_count; i++) {
        n += snprintf(text + n, sizeof text - (size_t)n, "%llu %lu %s\n",
                      (unsigned long long)entries[i].offset, (unsigned long)entries[i].size,
                      entries[i].path);
    }
    TEST_ASSERT_EQUAL_HEX32(xc_crc32(0, text, (size_t)n), base);
}

static void test_validators(void)
{
    static const char *const values[][2] = {
        {"", ""}, {"-", "-"}, {"W/\"x y\"", ""}, {"", "a%b"}, {"\xFF\x01", "\t"},
    };
    for (size_t i = 0; i < sizeof values / sizeof values[0]; i++) {
        xc_journal_make_id(&image, &listing, 0, 100, values[i][0], values[i][1]);
        used = 0;
        start();
        read_back(used);
        TEST_ASSERT_TRUE(st.valid);
        TEST_ASSERT_EQUAL_STRING(values[i][0], st.id.etag);
        TEST_ASSERT_EQUAL_STRING(values[i][1], st.id.last_modified);
    }
    /* NULL and too long mean "none". */
    char longer[200];
    memset(longer, 'v', sizeof longer - 1);
    longer[sizeof longer - 1] = '\0';
    xc_journal_make_id(&image, &listing, 0, 100, NULL, longer);
    TEST_ASSERT_EQUAL_STRING("", image.etag);
    TEST_ASSERT_EQUAL_STRING("", image.last_modified);
}

static void test_invalid_records(void)
{
    start();
    raw_line("done 12345 0000ABCD missing.bin");
    expect_invalid("doesn't have");

    used = 0;
    start();
    raw_line("done 12346 0000ABCD default.xbe"); /* wrong size */
    expect_invalid("doesn't have");

    used = 0;
    start();
    done(0, 1U);
    done(0, 1U);
    expect_invalid("twice");

    static const char *const malformed[] = {
        "done 12345 0000abcd default.xbe",
        "done 012345 0000ABCD default.xbe",
        "done 12345 0000ABCD",
        "done 12345 0000ABCD default.xbe extra",
        "done  12345 0000ABCD default.xbe",
        "done 12345 0000ABCD %4",
        "done 12345 0000ABCD a%00b",
        "done 4294967296 0000ABCD default.xbe",
    };
    for (size_t i = 0; i < sizeof malformed / sizeof malformed[0]; i++) {
        used = 0;
        start();
        raw_line(malformed[i]);
        expect_invalid("malformed");
    }

    used = 0;
    start();
    raw_line("hash sha1 0123");
    expect_invalid("unknown record");
}

static void test_newer_minor_records_are_skipped(void)
{
    start();
    /* Rewrite the header as v1.2, then add a record type a v1.1 reader doesn't know. */
    char rest[4096];
    char *nl = memchr(buf, '\n', used);
    TEST_ASSERT_NOT_NULL(nl);
    size_t tail = used - (size_t)(nl + 1 - buf);
    memcpy(rest, nl + 1, tail);
    used = 0;
    raw_line("# xcat-journal v1.2");
    memcpy(buf + used, rest, tail);
    used += tail;
    raw_line("hash sha1 0123");
    done(0, 7U);
    read_back(used);
    TEST_ASSERT_TRUE(st.valid);
    TEST_ASSERT_EQUAL_size_t(1, st.done_count);
}

static void test_bad_headers_and_sizes(void)
{
    static const char *const headers[] = {
        "# xcat-journal v2", "# xcat-journal v0", "# xcat-map v1", "#xcat-journal v1", "",
    };
    for (size_t i = 0; i < sizeof headers / sizeof headers[0]; i++) {
        used = 0;
        raw_line(headers[i]);
        expect_invalid("identity");
    }
    /* Identity lines out of order. */
    used = 0;
    start();
    char *image_line = strstr(buf, "\nimage ");
    TEST_ASSERT_NOT_NULL(image_line);
    image_line[1] = 'I';
    expect_invalid("identity"); /* the CRC no longer matches: torn inside the identity */
    /* Over 4 MiB. */
    xc_journal_state_free(&st);
    char *big = calloc(XC_JOURNAL_MAX_FILE + 1, 1);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_EQUAL_INT(XC_OK,
                          xc_journal_read(big, XC_JOURNAL_MAX_FILE + 1, &listing, &image, &st));
    TEST_ASSERT_FALSE(st.valid);
    TEST_ASSERT_NOT_NULL(strstr(st.why, "4 MiB"));
    free(big);
    /* A complete line over 8192 bytes. */
    used = 0;
    start();
    char line[XC_JOURNAL_MAX_LINE + 64];
    memset(line, 'x', sizeof line - 1);
    line[sizeof line - 1] = '\0';
    memcpy(line, "done 1 00000000 ", 16);
    raw_line(line);
    expect_invalid("overlong");
}

static void test_part_records(void)
{
    start();
    part(1, 67108864U, 0xAAAAAAAAU);
    part(1, 134217728U, 0xBBBBBBBBU);
    part(0, 4096U, 0x0000000CU);
    read_back(used);
    TEST_ASSERT_TRUE(st.valid);
    TEST_ASSERT_EQUAL_size_t(0, st.done_count);
    TEST_ASSERT_EQUAL_UINT32(134217728U, st.part_bytes[1]); /* the last checkpoint counts */
    TEST_ASSERT_EQUAL_HEX32(0xBBBBBBBBU, st.part_crc[1]);
    TEST_ASSERT_EQUAL_UINT32(4096U, st.part_bytes[0]);
    TEST_ASSERT_EQUAL_UINT32(0, st.part_bytes[2]);
    buf[used] = '\0';
    TEST_ASSERT_NOT_NULL(strstr(buf, "\npart 134217728 BBBBBBBB media/a%20b.bik *"));
    /* Finishing the file replaces its checkpoints. */
    done(1, 0x12345678U);
    read_back(used);
    TEST_ASSERT_TRUE(st.valid);
    TEST_ASSERT_TRUE(st.done[1]);
    TEST_ASSERT_EQUAL_UINT32(0, st.part_bytes[1]);
    TEST_ASSERT_EQUAL_UINT32(4096U, st.part_bytes[0]);

    static const char *const bad[] = {
        "part 0 0000000C default.xbe",     /* nothing yet: not a checkpoint */
        "part 12345 0000000C default.xbe", /* the whole file: that's a done line */
        "part 99999 0000000C default.xbe", /* past the end */
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        used = 0;
        start();
        raw_line(bad[i]);
        expect_invalid("checkpoint outside its file");
    }
    /* Going down is fine: a file written again from the start after a failed resume. */
    used = 0;
    start();
    part(0, 8192U, 1U);
    part(0, 4096U, 2U);
    read_back(used);
    TEST_ASSERT_TRUE(st.valid);
    TEST_ASSERT_EQUAL_UINT32(4096U, st.part_bytes[0]);
    TEST_ASSERT_EQUAL_HEX32(2U, st.part_crc[0]);
    used = 0;
    start();
    done(0, 1U);
    part(0, 4096U, 1U); /* after the file was done */
    expect_invalid("twice");
    used = 0;
    start();
    raw_line("part 10 0000000C nowhere.bin");
    expect_invalid("doesn't have");
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG,
                          xc_journal_format_part("a", 0, 1, buf, sizeof buf, &used));
}

static void test_format_limits(void)
{
    char small[32];
    size_t n = 99;
    TEST_ASSERT_EQUAL_INT(XC_ERR_LIMIT,
                          xc_journal_format_start(&image, "http://h/x", small, sizeof small, &n));
    TEST_ASSERT_EQUAL_size_t(0, n);
    TEST_ASSERT_EQUAL_INT(XC_ERR_LIMIT, xc_journal_format_done("a", 1, 2, small, 10, &n));
    /* A path whose encoded line would pass 8192 bytes. */
    static char path[3000];
    memset(path, ' ', sizeof path - 1);
    TEST_ASSERT_EQUAL_INT(XC_ERR_LIMIT, xc_journal_format_done(path, 1, 2, buf, sizeof buf, &n));
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG,
                          xc_journal_format_done("", 1, 2, buf, sizeof buf, &n));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_round_trip);
    RUN_TEST(test_torn_tail_at_every_byte);
    RUN_TEST(test_damaged_line_stops_reading);
    RUN_TEST(test_other_image);
    RUN_TEST(test_listing_crc);
    RUN_TEST(test_validators);
    RUN_TEST(test_invalid_records);
    RUN_TEST(test_newer_minor_records_are_skipped);
    RUN_TEST(test_bad_headers_and_sizes);
    RUN_TEST(test_part_records);
    RUN_TEST(test_format_limits);
    return UNITY_END();
}
