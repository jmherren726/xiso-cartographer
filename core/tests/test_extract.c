/* Tests for the extractor (xcat/extract.h, ADR 0004). Fixture extraction is tested end to end by
 * the extract_<fixture> tests (xcat-cli + tools/fixtures/extract_and_check.py). */
#include "unity.h"
#include "xcat/extract.h"

#include <stdbool.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* --- source: 64 KiB of a known pattern -------------------------------------- */

#define SRC_SIZE 65536U
static uint8_t src_data[SRC_SIZE];

static uint8_t pattern(size_t i)
{
    return (uint8_t)((i * 7U + 3U) & 0xFFU);
}

/* --- sink: records everything ----------------------------------------------- */

#define MAX_FILES 8
#define MAX_DATA 8192

typedef struct mfile {
    const char *path;
    uint8_t data[MAX_DATA];
    size_t len;
    uint32_t declared;
    bool ended;
    bool complete;
} mfile;

typedef struct msink {
    const char *dirs[8];
    size_t ndirs;
    mfile files[MAX_FILES];
    size_t nfiles;
    mfile *open;
    size_t writes_before_failure; /* SIZE_MAX = never fail */
    bool dirs_after_files;        /* set if a directory is made after a file begins */
} msink;

static msink ms;

static xc_err m_make_dir(void *ctx, const char *path)
{
    msink *m = ctx;
    if (m->nfiles > 0) {
        m->dirs_after_files = true;
    }
    m->dirs[m->ndirs++] = path;
    return XC_OK;
}

static xc_err m_begin(void *ctx, const char *path, uint32_t size)
{
    msink *m = ctx;
    TEST_ASSERT_NULL_MESSAGE(m->open, "begin_file while a file is open");
    TEST_ASSERT_TRUE(m->nfiles < MAX_FILES);
    mfile *f = &m->files[m->nfiles++];
    memset(f, 0, sizeof *f);
    f->path = path;
    f->declared = size;
    m->open = f;
    return XC_OK;
}

static xc_err m_write(void *ctx, const void *buf, size_t len)
{
    msink *m = ctx;
    TEST_ASSERT_NOT_NULL(m->open);
    if (m->writes_before_failure == 0) {
        return XC_ERR_IO;
    }
    if (m->writes_before_failure != SIZE_MAX) {
        m->writes_before_failure--;
    }
    TEST_ASSERT_TRUE(m->open->len + len <= MAX_DATA);
    memcpy(m->open->data + m->open->len, buf, len);
    m->open->len += len;
    return XC_OK;
}

static xc_err m_end(void *ctx, bool complete)
{
    msink *m = ctx;
    TEST_ASSERT_NOT_NULL(m->open);
    m->open->ended = true;
    m->open->complete = complete;
    m->open = NULL;
    return XC_OK;
}

/* --- helpers ------------------------------------------------------------------ */

static uint8_t buf[1000]; /* deliberately small: forces several chunks per file */

static xc_extract_opts opts(uint64_t gap)
{
    xc_extract_opts o = {buf, sizeof buf, gap, NULL, NULL};
    return o;
}

static xc_xdvdfs_entry file_at(const char *path, uint64_t offset, uint32_t size)
{
    xc_xdvdfs_entry e = {path, offset, size, (uint32_t)(offset / 2048U), 0x20};
    return e;
}

static xc_err extract(xc_xdvdfs_entry *files, size_t nfiles, xc_xdvdfs_entry *dirs, size_t ndirs,
                      const xc_extract_opts *o, xc_extract_stats *st)
{
    xc_mem_source mem;
    xc_source src;
    xc_source_memory(&src, &mem, src_data, sizeof src_data);
    xc_xdvdfs_listing l;
    memset(&l, 0, sizeof l);
    l.files = files;
    l.file_count = nfiles;
    l.dirs = dirs;
    l.dir_count = ndirs;
    for (size_t i = 0; i < nfiles; i++) {
        l.total_file_bytes += files[i].size;
    }
    memset(&ms, 0, sizeof ms);
    ms.writes_before_failure = SIZE_MAX;
    xc_sink sink = {m_make_dir, m_begin, m_write, m_end, &ms};
    return xc_extract(&src, &l, &sink, o, st);
}

static void assert_content(const mfile *f, uint64_t offset)
{
    TEST_ASSERT_TRUE(f->ended);
    TEST_ASSERT_TRUE(f->complete);
    TEST_ASSERT_EQUAL_size_t(f->declared, f->len);
    for (size_t i = 0; i < f->len; i++) {
        if (f->data[i] != pattern((size_t)offset + i)) {
            TEST_FAIL_MESSAGE(f->path);
        }
    }
}

/* --- tests -------------------------------------------------------------------- */

static void test_dirs_first_then_files_with_correct_bytes(void)
{
    xc_xdvdfs_entry dirs[] = {{"a", 0, 2048, 0, 0x10}, {"a/b", 0, 2048, 0, 0x10}};
    xc_xdvdfs_entry files[] = {file_at("a/one", 100, 50), file_at("a/b/two", 4096, 3000)};
    xc_extract_opts o = opts(XC_EXTRACT_DEFAULT_GAP);
    xc_extract_stats st;
    TEST_ASSERT_EQUAL_INT(XC_OK, extract(files, 2, dirs, 2, &o, &st));
    TEST_ASSERT_EQUAL_size_t(2, ms.ndirs);
    TEST_ASSERT_EQUAL_STRING("a", ms.dirs[0]);
    TEST_ASSERT_EQUAL_STRING("a/b", ms.dirs[1]);
    TEST_ASSERT_FALSE(ms.dirs_after_files);
    TEST_ASSERT_EQUAL_size_t(2, ms.nfiles);
    TEST_ASSERT_EQUAL_STRING("a/one", ms.files[0].path);
    assert_content(&ms.files[0], 100);
    assert_content(&ms.files[1], 4096);
    TEST_ASSERT_EQUAL_size_t(2, st.dirs_created);
    TEST_ASSERT_EQUAL_size_t(2, st.files_written);
    TEST_ASSERT_EQUAL_UINT64(3050, st.bytes_written);
}

static void test_small_gap_is_read_through_in_one_span(void)
{
    xc_xdvdfs_entry files[] = {file_at("a", 100, 50), file_at("b", 4096, 3000)};
    xc_extract_opts o = opts(XC_EXTRACT_DEFAULT_GAP);
    xc_extract_stats st;
    TEST_ASSERT_EQUAL_INT(XC_OK, extract(files, 2, NULL, 0, &o, &st));
    TEST_ASSERT_EQUAL_size_t(1, st.spans);
    TEST_ASSERT_EQUAL_UINT64(3050 + (4096 - 150), st.bytes_read); /* data + gap */
}

static void test_large_gap_starts_a_new_span(void)
{
    xc_xdvdfs_entry files[] = {file_at("a", 100, 50), file_at("b", 4096, 3000)};
    xc_extract_opts o = opts(1000);
    xc_extract_stats st;
    TEST_ASSERT_EQUAL_INT(XC_OK, extract(files, 2, NULL, 0, &o, &st));
    TEST_ASSERT_EQUAL_size_t(2, st.spans);
    TEST_ASSERT_EQUAL_UINT64(3050, st.bytes_read); /* nothing wasted */
    assert_content(&ms.files[1], 4096);
}

static void test_overlapping_files_restart_the_span(void)
{
    xc_xdvdfs_entry files[] = {file_at("a", 100, 200), file_at("b", 150, 50)};
    xc_extract_opts o = opts(XC_EXTRACT_DEFAULT_GAP);
    xc_extract_stats st;
    TEST_ASSERT_EQUAL_INT(XC_OK, extract(files, 2, NULL, 0, &o, &st));
    TEST_ASSERT_EQUAL_size_t(2, st.spans);
    assert_content(&ms.files[0], 100);
    assert_content(&ms.files[1], 150);
}

static void test_empty_file_reads_nothing(void)
{
    /* A zero-length file's sector is never checked, so its offset may be anywhere. */
    xc_xdvdfs_entry files[] = {file_at("empty", UINT64_MAX - 10, 0)};
    xc_extract_opts o = opts(XC_EXTRACT_DEFAULT_GAP);
    xc_extract_stats st;
    TEST_ASSERT_EQUAL_INT(XC_OK, extract(files, 1, NULL, 0, &o, &st));
    TEST_ASSERT_EQUAL_size_t(1, st.files_written);
    TEST_ASSERT_EQUAL_UINT64(0, st.bytes_read);
    TEST_ASSERT_EQUAL_size_t(0, st.spans);
    TEST_ASSERT_TRUE(ms.files[0].complete);
}

static int progress_calls;
static int cancel_after;

static bool on_progress(void *ctx, const xc_extract_progress *p)
{
    (void)ctx;
    TEST_ASSERT_TRUE(p->bytes_done <= p->bytes_total);
    TEST_ASSERT_TRUE(p->files_done <= p->file_count);
    progress_calls++;
    return progress_calls < cancel_after;
}

static void test_progress_reaches_totals(void)
{
    xc_xdvdfs_entry files[] = {file_at("a", 0, 2500), file_at("b", 3000, 10)};
    xc_extract_opts o = opts(XC_EXTRACT_DEFAULT_GAP);
    o.progress = on_progress;
    progress_calls = 0;
    cancel_after = 1000;
    xc_extract_stats st;
    TEST_ASSERT_EQUAL_INT(XC_OK, extract(files, 2, NULL, 0, &o, &st));
    TEST_ASSERT_EQUAL_INT(3 + 1 + 2, progress_calls); /* chunks of "a", chunk of "b", 2 file ends */
}

static void test_cancel_ends_the_open_file_incomplete(void)
{
    xc_xdvdfs_entry files[] = {file_at("a", 0, 2500), file_at("b", 3000, 10)};
    xc_extract_opts o = opts(XC_EXTRACT_DEFAULT_GAP);
    o.progress = on_progress;
    progress_calls = 0;
    cancel_after = 1; /* cancel at the first report */
    xc_extract_stats st;
    TEST_ASSERT_EQUAL_INT(XC_ERR_CANCELLED, extract(files, 2, NULL, 0, &o, &st));
    TEST_ASSERT_EQUAL_size_t(1, ms.nfiles);
    TEST_ASSERT_TRUE(ms.files[0].ended);
    TEST_ASSERT_FALSE(ms.files[0].complete);
    TEST_ASSERT_EQUAL_size_t(0, st.files_written);
}

static void test_sink_write_error_stops_and_ends_incomplete(void)
{
    xc_xdvdfs_entry files[] = {file_at("a", 0, 2500)};
    xc_mem_source mem;
    xc_source src;
    xc_source_memory(&src, &mem, src_data, sizeof src_data);
    xc_xdvdfs_listing l;
    memset(&l, 0, sizeof l);
    l.files = files;
    l.file_count = 1;
    memset(&ms, 0, sizeof ms);
    ms.writes_before_failure = 1; /* second write fails */
    xc_sink sink = {m_make_dir, m_begin, m_write, m_end, &ms};
    xc_extract_opts o = opts(XC_EXTRACT_DEFAULT_GAP);
    TEST_ASSERT_EQUAL_INT(XC_ERR_IO, xc_extract(&src, &l, &sink, &o, NULL));
    TEST_ASSERT_TRUE(ms.files[0].ended);
    TEST_ASSERT_FALSE(ms.files[0].complete);
}

static void test_source_error_propagates(void)
{
    xc_xdvdfs_entry files[] = {file_at("past-end", SRC_SIZE - 10, 100)};
    xc_extract_opts o = opts(XC_EXTRACT_DEFAULT_GAP);
    xc_extract_stats st;
    TEST_ASSERT_EQUAL_INT(XC_ERR_RANGE, extract(files, 1, NULL, 0, &o, &st));
    TEST_ASSERT_FALSE(ms.files[0].complete);
}

static void test_invalid_arguments(void)
{
    xc_xdvdfs_entry files[] = {file_at("a", 0, 1)};
    xc_extract_opts o = opts(XC_EXTRACT_DEFAULT_GAP);
    o.buf_size = 0;
    xc_extract_stats st;
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, extract(files, 1, NULL, 0, &o, &st));
    o = opts(XC_EXTRACT_DEFAULT_GAP);
    o.buf = NULL;
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, extract(files, 1, NULL, 0, &o, &st));
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_extract(NULL, NULL, NULL, NULL, NULL));
}

int main(void)
{
    for (size_t i = 0; i < SRC_SIZE; i++) {
        src_data[i] = pattern(i);
    }
    UNITY_BEGIN();
    RUN_TEST(test_dirs_first_then_files_with_correct_bytes);
    RUN_TEST(test_small_gap_is_read_through_in_one_span);
    RUN_TEST(test_large_gap_starts_a_new_span);
    RUN_TEST(test_overlapping_files_restart_the_span);
    RUN_TEST(test_empty_file_reads_nothing);
    RUN_TEST(test_progress_reaches_totals);
    RUN_TEST(test_cancel_ends_the_open_file_incomplete);
    RUN_TEST(test_sink_write_error_stops_and_ends_incomplete);
    RUN_TEST(test_source_error_propagates);
    RUN_TEST(test_invalid_arguments);
    return UNITY_END();
}
