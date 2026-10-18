/* Tests for the extractor (xcat/extract.h, ADR 0004). Fixture extraction is tested end to end by
 * the extract_<fixture> tests (xcat-cli + tools/fixtures/extract_and_check.py). */
#include "unity.h"
#include "xcat/crc32.h"
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
    size_t nwrites;
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
    m->nwrites++;
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
    xc_extract_opts o;
    memset(&o, 0, sizeof o);
    o.buf = buf;
    o.buf_size = sizeof buf;
    o.coalesce_gap = gap;
    return o;
}

static xc_xdvdfs_entry file_at(const char *path, uint64_t offset, uint32_t size)
{
    xc_xdvdfs_entry e = {path, offset, size, (uint32_t)(offset / 2048U), 0x20};
    return e;
}

/* on_bytes: checks that the bytes come in order with the source's content. */
typedef struct seen_log {
    uint64_t next;   /* where the next piece must start */
    uint64_t total;  /* bytes seen */
    bool backwards;  /* a piece started somewhere else */
    bool wrong_data; /* a piece didn't hold the source's bytes */
} seen_log;

static seen_log seen;

static xc_err on_bytes(void *ctx, uint64_t offset, const uint8_t *data, size_t len)
{
    seen_log *s = ctx;
    if (offset != s->next) {
        s->backwards = true;
    }
    for (size_t i = 0; i < len; i++) {
        s->wrong_data = s->wrong_data || data[i] != pattern((size_t)offset + i);
    }
    s->next = offset + len;
    s->total += len;
    return XC_OK;
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
    xc_sink sink = {m_make_dir, m_begin, m_write, m_end, &ms, NULL, NULL, NULL};
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

static xc_extract_opts whole_opts(uint64_t from)
{
    xc_extract_opts o = opts(16); /* tiny gap limit: the whole-image mode must ignore it */
    o.whole_image = true;
    o.whole_from = from;
    o.on_bytes = on_bytes;
    o.on_bytes_ctx = &seen;
    memset(&seen, 0, sizeof seen);
    seen.next = from;
    return o;
}

static void test_whole_image_reads_every_byte_in_order(void)
{
    /* Gaps far over the limit, a skipped file in the middle, nothing after 30000. */
    xc_xdvdfs_entry files[] = {file_at("one", 2048, 500), file_at("skip", 8192, 4000),
                               file_at("three", 20000, 3000), file_at("empty", 0, 0),
                               file_at("four", 27000, 3000)};
    bool skip[] = {false, true, false, false, false};
    xc_extract_opts o = whole_opts(0);
    o.skip = skip;
    xc_extract_stats st;
    TEST_ASSERT_EQUAL_INT(XC_OK, extract(files, 5, NULL, 0, &o, &st));
    TEST_ASSERT_FALSE(seen.backwards);
    TEST_ASSERT_FALSE(seen.wrong_data);
    TEST_ASSERT_EQUAL_UINT64(SRC_SIZE, seen.total); /* the tail after the last file too */
    TEST_ASSERT_EQUAL_UINT64(SRC_SIZE, st.bytes_read);
    TEST_ASSERT_EQUAL_size_t(4, ms.nfiles); /* "skip" isn't written */
    assert_content(&ms.files[0], 2048);
    assert_content(&ms.files[1], 20000);
    assert_content(&ms.files[3], 27000);
    TEST_ASSERT_EQUAL_size_t(2, st.spans); /* one through every file, one for the tail */
}

static void test_whole_image_from_an_offset(void)
{
    /* A resume (journal.md §6.7): "one" is done, the hash state stands at 2496. */
    xc_xdvdfs_entry files[] = {file_at("one", 2048, 500), file_at("two", 6144, 100)};
    bool skip[] = {true, false};
    xc_extract_opts o = whole_opts(2496);
    o.skip = skip;
    TEST_ASSERT_EQUAL_INT(XC_OK, extract(files, 2, NULL, 0, &o, NULL));
    TEST_ASSERT_FALSE(seen.backwards);
    TEST_ASSERT_EQUAL_UINT64(SRC_SIZE - 2496U, seen.total);
    TEST_ASSERT_EQUAL_size_t(1, ms.nfiles);
    assert_content(&ms.files[0], 6144);

    /* Everything done: only the rest of the image is read. */
    bool all[] = {true, true};
    o = whole_opts(60000);
    o.skip = all;
    TEST_ASSERT_EQUAL_INT(XC_OK, extract(files, 2, NULL, 0, &o, NULL));
    TEST_ASSERT_EQUAL_UINT64(SRC_SIZE - 60000U, seen.total);
    TEST_ASSERT_EQUAL_size_t(0, ms.nfiles);
}

static void test_whole_image_with_overlap_goes_back(void)
{
    /* Two entries sharing data can't both be fed by one forward read. */
    xc_xdvdfs_entry files[] = {file_at("a", 4096, 3000), file_at("b", 5000, 1000)};
    xc_extract_opts o = whole_opts(0);
    TEST_ASSERT_EQUAL_INT(XC_OK, extract(files, 2, NULL, 0, &o, NULL));
    TEST_ASSERT_TRUE(seen.backwards);
    assert_content(&ms.files[0], 4096);
    assert_content(&ms.files[1], 5000);
}

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
    xc_sink sink = {m_make_dir, m_begin, m_write, m_end, &ms, NULL, NULL, NULL};
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

/* --- resume: skip, file_done ------------------------------------------------------- */

static size_t done_index[MAX_FILES];
static uint32_t done_crc[MAX_FILES];
static size_t ndone;

static xc_err on_file_done(void *ctx, size_t index, uint32_t crc)
{
    (void)ctx;
    done_index[ndone] = index;
    done_crc[ndone] = crc;
    ndone++;
    return XC_OK;
}

static uint32_t expected_crc(uint64_t offset, uint32_t size)
{
    return xc_crc32(0, src_data + offset, size);
}

static bool progress_started_at_skipped;

static bool check_resume_progress(void *ctx, const xc_extract_progress *p)
{
    (void)ctx;
    /* "b" (500 bytes, file 1) was installed before: progress starts there. */
    progress_started_at_skipped =
        progress_started_at_skipped || (p->bytes_done > 500 && p->files_done >= 1);
    TEST_ASSERT_TRUE(p->bytes_done >= 500);
    TEST_ASSERT_TRUE(p->files_done >= 1);
    return true;
}

static void test_skip_and_file_done(void)
{
    xc_xdvdfs_entry files[] = {file_at("a", 0, 1500), file_at("b", 2048, 500),
                               file_at("empty", 0, 0), file_at("c", 4096, 300)};
    bool skip[] = {false, true, false, false};
    xc_extract_opts o = opts(XC_EXTRACT_DEFAULT_GAP);
    o.skip = skip;
    o.file_done = on_file_done;
    o.progress = check_resume_progress;
    ndone = 0;
    progress_started_at_skipped = false;
    xc_extract_stats st;
    TEST_ASSERT_EQUAL_INT(XC_OK, extract(files, 4, NULL, 0, &o, &st));
    TEST_ASSERT_EQUAL_size_t(3, ms.nfiles); /* "b" is never touched */
    TEST_ASSERT_EQUAL_STRING("a", ms.files[0].path);
    TEST_ASSERT_EQUAL_STRING("empty", ms.files[1].path);
    TEST_ASSERT_EQUAL_STRING("c", ms.files[2].path);
    assert_content(&ms.files[0], 0);
    assert_content(&ms.files[2], 4096);
    TEST_ASSERT_EQUAL_size_t(3, st.files_written);
    TEST_ASSERT_EQUAL_size_t(1, st.files_skipped);
    TEST_ASSERT_EQUAL_size_t(1, st.spans); /* "b"'s bytes are read through as part of the gap */
    TEST_ASSERT_EQUAL_size_t(3, ndone);
    TEST_ASSERT_EQUAL_size_t(0, done_index[0]);
    TEST_ASSERT_EQUAL_HEX32(expected_crc(0, 1500), done_crc[0]);
    TEST_ASSERT_EQUAL_size_t(2, done_index[1]);
    TEST_ASSERT_EQUAL_HEX32(0, done_crc[1]);
    TEST_ASSERT_EQUAL_size_t(3, done_index[2]);
    TEST_ASSERT_EQUAL_HEX32(expected_crc(4096, 300), done_crc[2]);
    TEST_ASSERT_TRUE(progress_started_at_skipped);
    /* Everything skipped: nothing is read. */
    bool all[] = {true, true, true, true};
    o.skip = all;
    o.progress = NULL;
    TEST_ASSERT_EQUAL_INT(XC_OK, extract(files, 4, NULL, 0, &o, &st));
    TEST_ASSERT_EQUAL_size_t(0, ms.nfiles);
    TEST_ASSERT_EQUAL_UINT64(0, st.bytes_read);
}

static xc_err fail_file_done(void *ctx, size_t index, uint32_t crc)
{
    (void)ctx;
    (void)crc;
    return index == 0 ? XC_ERR_IO : XC_OK;
}

static void test_file_done_error_stops(void)
{
    xc_xdvdfs_entry files[] = {file_at("a", 0, 100), file_at("b", 100, 100)};
    xc_extract_opts o = opts(XC_EXTRACT_DEFAULT_GAP);
    o.file_done = fail_file_done;
    xc_extract_stats st;
    TEST_ASSERT_EQUAL_INT(XC_ERR_IO, extract(files, 2, NULL, 0, &o, &st));
    TEST_ASSERT_EQUAL_size_t(1, ms.nfiles); /* "b" never begins */
    TEST_ASSERT_TRUE(ms.files[0].complete);
}

/* --- read_stream: one call per span, any piece size ------------------------------------ */

typedef struct stream_src {
    size_t piece;
    int calls;
    int read_at_calls;
    uint64_t skip_at; /* drop one byte at this offset (UINT64_MAX: never) */
    uint64_t stop_at; /* end the stream early here (UINT64_MAX: never) */
} stream_src;

static stream_src ss;

/* Must never be used while read_stream exists; the tests check read_at_calls stays 0. */
static xc_err ss_read_at(void *ctx, uint64_t offset, void *out, size_t len)
{
    stream_src *s = ctx;
    (void)offset;
    (void)out;
    (void)len;
    s->read_at_calls++;
    return XC_ERR_IO;
}

static xc_err ss_read_stream(void *ctx, uint64_t offset, uint64_t len, xc_source_data_fn fn,
                             void *fn_ctx)
{
    stream_src *s = ctx;
    s->calls++;
    uint64_t end = offset + len;
    for (uint64_t pos = offset; pos < end;) {
        if (pos >= s->stop_at) {
            return XC_OK; /* the source "forgot" the rest */
        }
        if (pos == s->skip_at) {
            pos++;
            continue;
        }
        size_t n = end - pos < s->piece ? (size_t)(end - pos) : s->piece;
        if (s->skip_at > pos && s->skip_at < pos + n) {
            n = (size_t)(s->skip_at - pos);
        }
        xc_err err = fn(fn_ctx, pos, src_data + pos, n);
        if (err != XC_OK) {
            return err;
        }
        pos += n;
    }
    return XC_OK;
}

static xc_err extract_streamed(xc_xdvdfs_entry *files, size_t nfiles, const xc_extract_opts *o,
                               xc_extract_stats *st)
{
    xc_source src = {ss_read_at, &ss, SRC_SIZE, ss_read_stream};
    xc_xdvdfs_listing l;
    memset(&l, 0, sizeof l);
    l.files = files;
    l.file_count = nfiles;
    for (size_t i = 0; i < nfiles; i++) {
        l.total_file_bytes += files[i].size;
    }
    memset(&ms, 0, sizeof ms);
    ms.writes_before_failure = SIZE_MAX;
    xc_sink sink = {m_make_dir, m_begin, m_write, m_end, &ms, NULL, NULL, NULL};
    return xc_extract(&src, &l, &sink, o, st);
}

static void reset_stream(size_t piece)
{
    memset(&ss, 0, sizeof ss);
    ss.piece = piece;
    ss.skip_at = UINT64_MAX;
    ss.stop_at = UINT64_MAX;
}

static void test_read_stream_routes_spans(void)
{
    xc_xdvdfs_entry files[] = {file_at("a", 0, 2500),      file_at("e1", 0, 0),
                               file_at("b", 2600, 7),      file_at("c", 3000, 1),
                               file_at("far", 30000, 900), file_at("e2", 0, 0)};
    for (size_t piece = 1; piece <= 4099; piece += 1366) {
        reset_stream(piece);
        xc_extract_opts o = opts(1024);
        o.file_done = on_file_done;
        ndone = 0;
        xc_extract_stats st;
        TEST_ASSERT_EQUAL_INT(XC_OK, extract_streamed(files, 6, &o, &st));
        TEST_ASSERT_EQUAL_INT(2, ss.calls); /* [a..c] and [far]: one request each */
        TEST_ASSERT_EQUAL_INT(0, ss.read_at_calls);
        TEST_ASSERT_EQUAL_size_t(2, st.spans);
        TEST_ASSERT_EQUAL_size_t(6, ms.nfiles);
        assert_content(&ms.files[0], 0);
        assert_content(&ms.files[2], 2600);
        assert_content(&ms.files[3], 3000);
        assert_content(&ms.files[4], 30000);
        TEST_ASSERT_TRUE(ms.files[1].complete && ms.files[5].complete);
        TEST_ASSERT_EQUAL_UINT64(3001 + 900, st.bytes_read);
        TEST_ASSERT_EQUAL_size_t(6, ndone);
        TEST_ASSERT_EQUAL_HEX32(expected_crc(0, 2500), done_crc[0]);
        TEST_ASSERT_EQUAL_HEX32(expected_crc(30000, 900), done_crc[4]);
    }
    /* Small pieces are gathered: "a" (2500 bytes) arrives 1 byte at a time but is written in
       buffer-sized writes (1000, 1000, 500). */
    xc_xdvdfs_entry one[] = {file_at("a", 0, 2500)};
    reset_stream(1);
    xc_extract_opts o = opts(1024);
    xc_extract_stats st;
    TEST_ASSERT_EQUAL_INT(XC_OK, extract_streamed(one, 1, &o, &st));
    TEST_ASSERT_EQUAL_size_t(3, ms.nwrites);
    assert_content(&ms.files[0], 0);
}

static void test_read_stream_faults(void)
{
    xc_xdvdfs_entry files[] = {file_at("a", 0, 2500), file_at("b", 2600, 100)};
    xc_extract_opts o = opts(1024);
    xc_extract_stats st;
    /* A source that skips a byte must never produce a file with a hole. */
    reset_stream(64);
    ss.skip_at = 1000;
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, extract_streamed(files, 2, &o, &st));
    TEST_ASSERT_TRUE(ms.files[0].ended);
    TEST_ASSERT_FALSE(ms.files[0].complete);
    /* A stream that ends early leaves the open file incomplete. */
    reset_stream(64);
    ss.stop_at = 2650;
    TEST_ASSERT_EQUAL_INT(XC_ERR_IO, extract_streamed(files, 2, &o, &st));
    TEST_ASSERT_TRUE(ms.files[0].complete);
    TEST_ASSERT_TRUE(ms.files[1].ended);
    TEST_ASSERT_FALSE(ms.files[1].complete);
    /* A span past the end of the source is refused before anything is read. */
    xc_xdvdfs_entry past[] = {file_at("past", SRC_SIZE - 10, 100)};
    reset_stream(64);
    TEST_ASSERT_EQUAL_INT(XC_ERR_RANGE, extract_streamed(past, 1, &o, &st));
    TEST_ASSERT_EQUAL_INT(0, ss.calls);
}

int main(void)
{
    for (size_t i = 0; i < SRC_SIZE; i++) {
        src_data[i] = pattern(i);
    }
    UNITY_BEGIN();
    RUN_TEST(test_whole_image_reads_every_byte_in_order);
    RUN_TEST(test_whole_image_from_an_offset);
    RUN_TEST(test_whole_image_with_overlap_goes_back);
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
    RUN_TEST(test_skip_and_file_done);
    RUN_TEST(test_file_done_error_stops);
    RUN_TEST(test_read_stream_routes_spans);
    RUN_TEST(test_read_stream_faults);
    return UNITY_END();
}
