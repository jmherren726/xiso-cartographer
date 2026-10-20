/* Tests for sequential extraction (xcat/seq.h, docs/formats/stream-install.md). */
#include "unity.h"
#include "xcat/crc32.h"
#include "xcat/host.h"
#include "xcat/seq.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static const char *fixtures_dir;

/* --- the image, streamed from the fixture file --------------------------------------- */

static xc_host_file image_file;
static xc_source image;
static bool image_open;
static uint64_t image_size;
static uint64_t checkpoint_step; /* the stream restarts at multiples of this */
static unsigned streams;         /* stream calls (one per pass) */
static uint64_t first_from[64];  /* `from` of each call */

static void open_image(const char *name)
{
    char path[1024];
    (void)snprintf(path, sizeof path, "%s/%s.iso", fixtures_dir, name);
    if (image_open) {
        xc_host_file_close(&image_file);
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(XC_OK, xc_host_file_open(&image, &image_file, path), path);
    image_open = true;
    image_size = image.size;
    checkpoint_step = 65536;
    streams = 0;
}

static void read_image(uint64_t off, void *buf, size_t len)
{
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_source_read(&image, off, buf, len));
}

static xc_err stream(void *ctx, uint64_t from, xc_source_data_fn fn, void *fn_ctx)
{
    (void)ctx;
    TEST_ASSERT_TRUE(streams < 64);
    first_from[streams++] = from;
    static uint8_t buf[100003]; /* odd piece size */
    uint64_t off = from - from % checkpoint_step;
    while (off < image_size) {
        size_t n = (size_t)(image_size - off < sizeof buf ? image_size - off : sizeof buf);
        read_image(off, buf, n);
        xc_err err = fn(fn_ctx, off, buf, n);
        if (err != XC_OK) {
            return err;
        }
        off += n;
    }
    return XC_OK;
}

/* --- a memory sink ------------------------------------------------------------------ */

typedef struct mfile {
    char path[512];
    uint8_t *data;
    uint32_t size;
    uint32_t len;
    bool complete;
} mfile;

#define MAX_FILES 512
static mfile files[MAX_FILES];
static size_t nfiles;
static char dirs[MAX_FILES][512];
static size_t ndirs;
static mfile *open_file;
static uint64_t disk_used; /* files + spool */
static uint64_t disk_size;

static bool dir_exists(const char *path, size_t len)
{
    if (len == 0) {
        return true;
    }
    for (size_t i = 0; i < ndirs; i++) {
        if (strlen(dirs[i]) == len && memcmp(dirs[i], path, len) == 0) {
            return true;
        }
    }
    return false;
}

static size_t parent_len(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash != NULL ? (size_t)(slash - path) : 0;
}

static xc_err m_make_dir(void *ctx, const char *path)
{
    (void)ctx;
    TEST_ASSERT_TRUE_MESSAGE(dir_exists(path, parent_len(path)), path);
    TEST_ASSERT_FALSE_MESSAGE(dir_exists(path, strlen(path)), path);
    TEST_ASSERT_TRUE(ndirs < MAX_FILES);
    (void)snprintf(dirs[ndirs++], sizeof dirs[0], "%s", path);
    return XC_OK;
}

static xc_err m_begin(void *ctx, const char *path, uint32_t size)
{
    (void)ctx;
    TEST_ASSERT_NULL(open_file);
    TEST_ASSERT_TRUE_MESSAGE(dir_exists(path, parent_len(path)), path);
    for (size_t i = 0; i < nfiles; i++) {
        TEST_ASSERT_TRUE_MESSAGE(strcmp(files[i].path, path) != 0 || !files[i].complete, path);
    }
    TEST_ASSERT_TRUE(nfiles < MAX_FILES);
    open_file = &files[nfiles++];
    (void)snprintf(open_file->path, sizeof open_file->path, "%s", path);
    open_file->data = malloc(size > 0 ? size : 1U);
    TEST_ASSERT_NOT_NULL(open_file->data);
    open_file->size = size;
    open_file->len = 0;
    open_file->complete = false;
    disk_used += size;
    return XC_OK;
}

static xc_err m_write(void *ctx, const void *buf, size_t len)
{
    (void)ctx;
    TEST_ASSERT_NOT_NULL(open_file);
    TEST_ASSERT_TRUE(len <= open_file->size - open_file->len);
    memcpy(open_file->data + open_file->len, buf, len);
    open_file->len += (uint32_t)len;
    return XC_OK;
}

static xc_err m_end(void *ctx, bool complete)
{
    (void)ctx;
    TEST_ASSERT_NOT_NULL(open_file);
    if (complete) {
        TEST_ASSERT_EQUAL_UINT32(open_file->size, open_file->len);
    }
    open_file->complete = complete;
    open_file = NULL;
    return XC_OK;
}

static const xc_sink sink = {m_make_dir, m_begin, m_write, m_end, NULL, NULL, NULL, NULL};

/* --- a memory spool ----------------------------------------------------------------- */

#define MAX_CHUNKS 4096
static uint8_t *chunk_data[MAX_CHUNKS];
static uint64_t chunk_len[MAX_CHUNKS];
static uint64_t chunk_cap[MAX_CHUNKS];

static xc_err s_append(void *ctx, uint32_t c, const uint8_t *data, size_t len)
{
    (void)ctx;
    TEST_ASSERT_TRUE(c < MAX_CHUNKS);
    TEST_ASSERT_TRUE_MESSAGE(disk_used + len <= disk_size, "spool past the disk's end");
    if (chunk_len[c] + len > chunk_cap[c]) {
        uint64_t cap = (chunk_len[c] + len) * 2;
        uint8_t *p = realloc(chunk_data[c], (size_t)cap);
        TEST_ASSERT_NOT_NULL(p);
        chunk_data[c] = p;
        chunk_cap[c] = cap;
    }
    memcpy(chunk_data[c] + chunk_len[c], data, len);
    chunk_len[c] += len;
    disk_used += len;
    return XC_OK;
}

static xc_err s_read(void *ctx, uint32_t c, uint64_t pos, uint8_t *buf, size_t len)
{
    (void)ctx;
    TEST_ASSERT_TRUE(c < MAX_CHUNKS && pos + len <= chunk_len[c]);
    memcpy(buf, chunk_data[c] + pos, len);
    return XC_OK;
}

static xc_err s_drop(void *ctx, uint32_t c)
{
    (void)ctx;
    TEST_ASSERT_TRUE(c < MAX_CHUNKS);
    disk_used -= chunk_len[c];
    free(chunk_data[c]);
    chunk_data[c] = NULL;
    chunk_len[c] = 0;
    chunk_cap[c] = 0;
    return XC_OK;
}

static xc_err s_free(void *ctx, uint64_t *bytes)
{
    (void)ctx;
    *bytes = disk_size - disk_used;
    return XC_OK;
}

static const xc_seq_spool spool = {s_append, s_read, s_drop, s_free, NULL};

/* --- callbacks ---------------------------------------------------------------------- */

static unsigned complete_listings;
static size_t file_done_count;
static bool crc_ok;
static unsigned cancel_after; /* progress calls before cancelling; 0 = never */
static unsigned progress_calls;

static xc_err on_listing(void *ctx, const xc_xdvdfs_volume *vol, const xc_xdvdfs_listing *l,
                         bool complete)
{
    (void)ctx;
    (void)vol;
    TEST_ASSERT_EQUAL(complete, l->pending_count == 0);
    complete_listings += complete;
    return XC_OK;
}

static xc_err file_done(void *ctx, const xc_xdvdfs_entry *f, uint32_t crc)
{
    (void)ctx;
    file_done_count++;
    uint8_t *buf = malloc(f->size > 0 ? f->size : 1U);
    TEST_ASSERT_NOT_NULL(buf);
    if (f->size > 0) {
        read_image(f->offset, buf, f->size);
    }
    crc_ok &= crc == xc_crc32(0, buf, f->size);
    free(buf);
    return XC_OK;
}

static bool on_progress(void *ctx, const xc_seq_progress *p)
{
    (void)ctx;
    TEST_ASSERT_TRUE(p->offset <= p->image_size);
    progress_calls++;
    return cancel_after == 0 || progress_calls < cancel_after;
}

/* --- running it --------------------------------------------------------------------- */

static uint8_t io_buf[256 * 1024];

static void reset(void)
{
    for (size_t i = 0; i < nfiles; i++) {
        free(files[i].data);
    }
    memset(files, 0, sizeof files);
    nfiles = 0;
    ndirs = 0;
    open_file = NULL;
    for (size_t c = 0; c < MAX_CHUNKS; c++) {
        free(chunk_data[c]);
        chunk_data[c] = NULL;
        chunk_len[c] = chunk_cap[c] = 0;
    }
    disk_used = 0;
    complete_listings = 0;
    file_done_count = 0;
    crc_ok = true;
    cancel_after = 0;
    progress_calls = 0;
}

static xc_seq_req request(uint64_t chunk, uint64_t reserve)
{
    xc_seq_req q;
    memset(&q, 0, sizeof q);
    q.image_size = image_size;
    q.stream = stream;
    q.sink = &sink;
    q.spool = &spool;
    q.chunk_bytes = chunk;
    q.reserve = reserve;
    q.buf = io_buf;
    q.buf_size = sizeof io_buf;
    q.on_listing = on_listing;
    q.file_done = file_done;
    q.progress = on_progress;
    return q;
}

/* Every file in the fixture's manifest is there, complete, with the image's bytes. */
static void check_installed(const char *name)
{
    char path[1024];
    (void)snprintf(path, sizeof path, "%s/%s.tsv", fixtures_dir, name);
    FILE *tsv = fopen(path, "r");
    TEST_ASSERT_NOT_NULL(tsv);
    char line[2048];
    size_t want_files = 0;
    while (fgets(line, sizeof line, tsv) != NULL) {
        /* file|dir <offset> <size> <sector> <path>, tab-separated */
        char *kind = line;
        char *f_off = strchr(kind, '\t');
        char *f_size = f_off != NULL ? strchr(f_off + 1, '\t') : NULL;
        char *f_sector = f_size != NULL ? strchr(f_size + 1, '\t') : NULL;
        char *p = f_sector != NULL ? strchr(f_sector + 1, '\t') : NULL;
        if (p == NULL) {
            continue;
        }
        *f_off++ = '\0';
        *f_size++ = '\0';
        *p++ = '\0';
        char *nl = strpbrk(p, "\r\n");
        if (nl != NULL) {
            *nl = '\0';
        }
        uint64_t off = strtoull(f_off, NULL, 10);
        uint64_t size = strtoull(f_size, NULL, 10);
        TEST_ASSERT_TRUE(size <= 0xFFFFFFFFU);
        if (strcmp(kind, "dir") == 0) {
            TEST_ASSERT_TRUE_MESSAGE(dir_exists(p, strlen(p)), p);
            continue;
        }
        want_files++;
        const mfile *f = NULL;
        for (size_t i = 0; i < nfiles; i++) {
            if (files[i].complete && strcmp(files[i].path, p) == 0) {
                f = &files[i];
            }
        }
        TEST_ASSERT_NOT_NULL_MESSAGE(f, p);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)size, f->len, p);
        if (size > 0) {
            uint8_t *buf = malloc((size_t)size);
            TEST_ASSERT_NOT_NULL(buf);
            read_image(off, buf, (size_t)size);
            TEST_ASSERT_EQUAL_MEMORY_MESSAGE(buf, f->data, (size_t)size, p);
            free(buf);
        }
    }
    TEST_ASSERT_EQUAL_INT(0, fclose(tsv));
    TEST_ASSERT_EQUAL_size_t(want_files, file_done_count);
    TEST_ASSERT_TRUE(crc_ok);
    TEST_ASSERT_EQUAL_UINT(1, complete_listings);
    for (size_t c = 0; c < MAX_CHUNKS; c++) {
        TEST_ASSERT_EQUAL_UINT64(0, chunk_len[c]); /* the spool is gone */
    }
}

static xc_seq_result install(const char *name, uint64_t disk, uint64_t chunk, uint64_t reserve,
                             unsigned max_passes, xc_err want)
{
    reset();
    open_image(name);
    disk_size = disk;
    xc_seq_req q = request(chunk, reserve);
    q.max_passes = max_passes;
    xc_seq_result r;
    xc_err got = xc_seq_extract(&q, &r);
    if (got != want) {
        char msg[512];
        (void)snprintf(msg, sizeof msg,
                       "%s: passes %u, needed %llu, free %llu, file %s, spooled %llu, dropped %llu",
                       name, r.passes, (unsigned long long)r.space_needed,
                       (unsigned long long)r.space_free, r.current_file,
                       (unsigned long long)r.spooled, (unsigned long long)r.spool_dropped);
        TEST_FAIL_MESSAGE(msg);
    }
    TEST_ASSERT_EQUAL_UINT(streams, r.passes);
    if (want == XC_OK) {
        check_installed(name);
    }
    return r;
}

#define BIG ((uint64_t)1 << 40)
#define LATE_FILES 197333U      /* bytes of file data in "late" */
#define LATE_DEEP_FILES 707154U /* ...and in "late-deep" */

/* --- tests -------------------------------------------------------------------------- */

static void test_tables_first_one_pass(void)
{
    static const char *names[] = {"small", "deep", "redump-xgd3", "redump-xgd1"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        xc_seq_result r = install(names[i], BIG, 0, 0, 0, XC_OK);
        TEST_ASSERT_EQUAL_UINT(1, r.passes);
        TEST_ASSERT_EQUAL_size_t(0, r.files_from_spool);
        TEST_ASSERT_EQUAL_UINT64(0, r.reread);
    }
}

static void test_late_from_spool(void)
{
    static const char *names[] = {"late", "late-deep", "late-xgd3", "late-root"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        xc_seq_result r = install(names[i], BIG, 65536, 0, 0, XC_OK);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1, r.passes, names[i]);
        TEST_ASSERT_TRUE(r.files_from_spool > 0);
        TEST_ASSERT_TRUE(r.files_written <= 1); /* only the empty file */
        TEST_ASSERT_EQUAL_size_t(r.file_count, r.files_written + r.files_from_spool);
        TEST_ASSERT_TRUE(r.spool_peak > 0);
        TEST_ASSERT_EQUAL_UINT64(0, r.spool_dropped);
    }
}

static void test_tight_disk_needs_a_second_pass(void)
{
    /* the game's files plus 256 KiB of spool: the spool keeps only the latest bytes */
    xc_seq_result r = install("late", LATE_FILES + 262144 + 65536, 65536, 65536, 0, XC_OK);
    TEST_ASSERT_TRUE(r.passes >= 2);
    TEST_ASSERT_TRUE(r.spool_dropped > 0);
    TEST_ASSERT_TRUE(r.reread > 0);
    TEST_ASSERT_TRUE(first_from[1] > 0);
}

static void test_disk_just_big_enough(void)
{
    /* 4 KiB more than the game's files: the spool borrows the space the files will need,
       keeps the latest bytes, and gives it back chunk by chunk as files are copied out */
    xc_seq_result r = install("late-deep", LATE_DEEP_FILES + 4096, 65536, 0, 0, XC_OK);
    TEST_ASSERT_TRUE(r.passes >= 2);
    TEST_ASSERT_TRUE(r.files_from_spool > 0);
    TEST_ASSERT_TRUE(r.spool_dropped > 0);
}

static void test_tables_dropped_before_their_parent(void)
{
    /* "late-root": every subdirectory table comes first, deepest first, the root last. A
       small spool drops the early tables; pass 2 starts at the first table the root lists,
       spools the deeper ones it passes on the way, and fills them in from the spool once
       their parents arrive. */
    xc_seq_result r = install("late-root", LATE_DEEP_FILES + 4096, 65536, 0, 64, XC_OK);
    char msg[64];
    (void)snprintf(msg, sizeof msg, "passes %u, dropped %llu", r.passes,
                   (unsigned long long)r.spool_dropped);
    TEST_ASSERT_TRUE_MESSAGE(r.passes >= 2 && r.reread > 0, msg);
}

static void test_pass_limit(void)
{
    (void)install("late-deep", LATE_DEEP_FILES + 4096, 65536, 0, 1, XC_ERR_LIMIT);
}

static void test_no_space(void)
{
    xc_seq_result r = install("small", 100000, 0, 0, 0, XC_ERR_NO_SPACE);
    TEST_ASSERT_TRUE(r.space_needed > 0);
    TEST_ASSERT_TRUE(r.current_file[0] != '\0');
}

static void test_cancel(void)
{
    reset();
    open_image("late-xgd3");
    disk_size = BIG;
    cancel_after = 3;
    xc_seq_req q = request(0, 0);
    TEST_ASSERT_EQUAL_INT(XC_ERR_CANCELLED, xc_seq_extract(&q, NULL));
}

/* A memory image that holds no volume. */
static uint8_t blank[3U * 1024 * 1024];

static xc_err blank_stream(void *ctx, uint64_t from, xc_source_data_fn fn, void *fn_ctx)
{
    (void)ctx;
    return fn(fn_ctx, from, blank + from, sizeof blank - (size_t)from);
}

static void test_not_xdvdfs(void)
{
    reset();
    disk_size = BIG;
    xc_seq_req q = request(0, 0);
    q.image_size = sizeof blank;
    q.stream = blank_stream;
    TEST_ASSERT_EQUAL_INT(XC_ERR_NOT_XDVDFS, xc_seq_extract(&q, NULL));
}

static void test_invalid_arguments(void)
{
    xc_seq_req q = request(0, 0);
    q.buf_size = 1000;
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_seq_extract(&q, NULL));
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_seq_extract(NULL, NULL));
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        (void)fprintf(stderr, "usage: test_seq <fixtures-dir>\n");
        return 2;
    }
    fixtures_dir = argv[1];
    UNITY_BEGIN();
    RUN_TEST(test_tables_first_one_pass);
    RUN_TEST(test_late_from_spool);
    RUN_TEST(test_tight_disk_needs_a_second_pass);
    RUN_TEST(test_disk_just_big_enough);
    RUN_TEST(test_tables_dropped_before_their_parent);
    RUN_TEST(test_pass_limit);
    RUN_TEST(test_no_space);
    RUN_TEST(test_cancel);
    RUN_TEST(test_not_xdvdfs);
    RUN_TEST(test_invalid_arguments);
    reset();
    if (image_open) {
        xc_host_file_close(&image_file);
    }
    return UNITY_END();
}
