/*
 * Tests for xc_install (core/src/install.c) over an in-memory file system, with the
 * "small" fixture as the image (argv[1] is the fixtures directory). The cases that are
 * hard to produce on a real disk live here: no space, no folder name left, cancel and
 * resume, a failing journal, a finished file that went missing. The happy paths over
 * HTTP, kills and damaged files are tested end to end (tools/testserver/test_install.py).
 */
#include "unity.h"
#include "xcat/host.h"
#include "xcat/install.h"
#include "xcat/journal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *fixtures_dir = "build/fixtures";

/* --- a tiny in-memory file system ----------------------------------------------------- */

#define MAX_NODES 256

typedef struct node {
    char path[XC_INSTALL_MAX_PATH];
    bool dir;
    uint8_t *data;
    size_t len;
} node;

typedef struct memfs {
    node nodes[MAX_NODES];
    size_t count;
    uint64_t free_bytes;
    int appends_before_failure; /* < 0: never fail */
    node *open;                 /* the sink's open file */
    char sink_root[XC_INSTALL_MAX_PATH];
    xc_mem_source mem;
} memfs;

static memfs fs;

static node *find(const char *path)
{
    for (size_t i = 0; i < fs.count; i++) {
        if (strcmp(fs.nodes[i].path, path) == 0) {
            return &fs.nodes[i];
        }
    }
    return NULL;
}

static node *create(const char *path, bool dir)
{
    node *n = find(path);
    if (n == NULL) {
        TEST_ASSERT_TRUE(fs.count < MAX_NODES);
        n = &fs.nodes[fs.count++];
        memset(n, 0, sizeof *n);
        (void)snprintf(n->path, sizeof n->path, "%s", path);
    }
    n->dir = dir;
    free(n->data);
    n->data = NULL;
    n->len = 0;
    return n;
}

static void drop(const char *path)
{
    node *n = find(path);
    if (n != NULL) {
        free(n->data);
        *n = fs.nodes[--fs.count];
    }
}

static void add_bytes(node *n, const void *data, size_t len)
{
    uint8_t *grown = realloc(n->data, n->len + len + 1);
    TEST_ASSERT_NOT_NULL(grown);
    memcpy(grown + n->len, data, len);
    n->data = grown;
    n->len += len;
}

static bool f_exists(void *ctx, const char *path)
{
    (void)ctx;
    return find(path) != NULL;
}

static xc_err f_size(void *ctx, const char *path, uint64_t *size)
{
    (void)ctx;
    node *n = find(path);
    if (n == NULL || n->dir) {
        return XC_ERR_IO;
    }
    *size = n->len;
    return XC_OK;
}

static xc_err f_read_all(void *ctx, const char *path, size_t max, char **data, size_t *len)
{
    (void)ctx;
    node *n = find(path);
    if (n == NULL || n->dir) {
        return XC_ERR_IO;
    }
    if (n->len > max) {
        return XC_ERR_LIMIT;
    }
    *data = malloc(n->len + 1);
    TEST_ASSERT_NOT_NULL(*data);
    if (n->len > 0) {
        memcpy(*data, n->data, n->len);
    }
    *len = n->len;
    return XC_OK;
}

static xc_err f_append(void *ctx, const char *path, const void *data, size_t len)
{
    (void)ctx;
    if (fs.appends_before_failure == 0) {
        return XC_ERR_IO;
    }
    if (fs.appends_before_failure > 0) {
        fs.appends_before_failure--;
    }
    node *n = find(path);
    if (n == NULL) {
        n = create(path, false);
    }
    add_bytes(n, data, len);
    return XC_OK;
}

static xc_err f_write_all(void *ctx, const char *path, const void *data, size_t len)
{
    (void)ctx;
    add_bytes(create(path, false), data, len);
    return XC_OK;
}

static xc_err f_remove(void *ctx, const char *path)
{
    (void)ctx;
    drop(path);
    return XC_OK;
}

static xc_err f_free(void *ctx, const char *dir, uint64_t *bytes)
{
    (void)ctx;
    (void)dir;
    *bytes = fs.free_bytes;
    return XC_OK;
}

/* The sink: paths below the root, '/' turned into the io's separator ('/'). */
static void sink_path(char *out, const char *rel)
{
    int n = snprintf(out, XC_INSTALL_MAX_PATH, "%s/%s", fs.sink_root, rel);
    TEST_ASSERT_TRUE(n > 0 && n < XC_INSTALL_MAX_PATH);
}

static xc_err s_make_dir(void *ctx, const char *path)
{
    (void)ctx;
    char full[XC_INSTALL_MAX_PATH];
    sink_path(full, path);
    if (find(full) == NULL) {
        (void)create(full, true);
    }
    return XC_OK;
}

static xc_err s_begin(void *ctx, const char *path, uint32_t size)
{
    (void)ctx;
    (void)size;
    char full[XC_INSTALL_MAX_PATH];
    sink_path(full, path);
    fs.open = create(full, false);
    return XC_OK;
}

static xc_err s_write(void *ctx, const void *buf, size_t len)
{
    (void)ctx;
    add_bytes(fs.open, buf, len);
    return XC_OK;
}

static xc_err s_end(void *ctx, bool complete)
{
    (void)ctx;
    if (!complete) {
        drop(fs.open->path);
    }
    fs.open = NULL;
    return XC_OK;
}

static int syncs;

static xc_err s_sync(void *ctx)
{
    (void)ctx;
    TEST_ASSERT_NOT_NULL(fs.open);
    syncs++;
    return XC_OK;
}

static xc_err s_resume(void *ctx, const char *path, uint32_t size, uint32_t keep)
{
    (void)ctx;
    (void)size;
    char full[XC_INSTALL_MAX_PATH];
    sink_path(full, path);
    node *n = find(full);
    if (n == NULL || n->dir || n->len < keep) {
        return XC_ERR_IO;
    }
    n->len = keep; /* cut back to the checkpoint */
    fs.open = n;
    return XC_OK;
}

static xc_err s_suspend(void *ctx)
{
    (void)ctx;
    TEST_ASSERT_NOT_NULL(fs.open);
    fs.open = NULL; /* closed, contents kept */
    return XC_OK;
}

static xc_err f_open_sink(void *ctx, const char *root, xc_sink *sink, void **handle)
{
    (void)ctx;
    (void)snprintf(fs.sink_root, sizeof fs.sink_root, "%s", root);
    if (find(root) == NULL) {
        (void)create(root, true);
    }
    sink->make_dir = s_make_dir;
    sink->begin_file = s_begin;
    sink->write = s_write;
    sink->end_file = s_end;
    sink->ctx = NULL;
    sink->sync = s_sync;
    sink->resume_file = s_resume;
    sink->suspend_file = s_suspend;
    *handle = NULL;
    return XC_OK;
}

static void f_close_sink(void *ctx, void *handle)
{
    (void)ctx;
    (void)handle;
}

static xc_err f_open_file(void *ctx, const char *path, xc_source *src, void **handle)
{
    (void)ctx;
    node *n = find(path);
    if (n == NULL || n->dir) {
        return XC_ERR_IO;
    }
    xc_source_memory(src, &fs.mem, n->data, n->len);
    *handle = NULL;
    return XC_OK;
}

static void f_close_file(void *ctx, void *handle)
{
    (void)ctx;
    (void)handle;
}

static const xc_install_io memio = {
    '/',    f_exists,    f_size,       f_read_all,  f_append,     f_write_all, f_remove,
    f_free, f_open_sink, f_close_sink, f_open_file, f_close_file, NULL,
};

/* --- the image and the request -------------------------------------------------------- */

static xc_host_file image_file;
static xc_source image;
static uint8_t buf[256 * 1024];
static xc_install_req req;
static xc_install_result res;
static int cancel_after;      /* progress reports while writing before cancelling; < 0: never */
static const char *cancel_in; /* or: cancel inside this file ... */
static int cancel_in_reports; /* ... after this many reports on it */

static bool on_progress(void *ctx, const xc_install_progress *p)
{
    (void)ctx;
    if (cancel_in != NULL && p->phase == XC_INSTALL_WRITING && p->current != NULL &&
        strcmp(p->current, cancel_in) == 0 && cancel_in_reports-- <= 0) {
        return false;
    }
    if (p->phase == XC_INSTALL_WRITING && cancel_after >= 0) {
        if (cancel_after == 0) {
            return false;
        }
        cancel_after--;
    }
    return true;
}

void setUp(void)
{
    for (size_t i = 0; i < fs.count; i++) {
        free(fs.nodes[i].data);
    }
    memset(&fs, 0, sizeof fs);
    fs.free_bytes = UINT64_MAX;
    fs.appends_before_failure = -1;
    cancel_after = -1;
    cancel_in = NULL;
    syncs = 0;
    memset(&req, 0, sizeof req);
    req.src = &image;
    req.source_name = "small.iso";
    req.games_dir = "E:/Games";
    req.title = "Small Game";
    req.io = &memio;
    req.buf = buf;
    req.buf_size = sizeof buf;
    req.cluster_size = 16384;
    req.progress = on_progress;
}

void tearDown(void) {}

static size_t count_prefix(const char *prefix)
{
    size_t n = 0;
    for (size_t i = 0; i < fs.count; i++) {
        n += strncmp(fs.nodes[i].path, prefix, strlen(prefix)) == 0 && !fs.nodes[i].dir ? 1U : 0U;
    }
    return n;
}

/* --- tests ------------------------------------------------------------------------------ */

static void test_fresh_install(void)
{
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_install(&req, &res));
    TEST_ASSERT_EQUAL_STRING("E:/Games/Small Game", res.folder);
    TEST_ASSERT_FALSE(res.resumed);
    TEST_ASSERT_EQUAL_size_t(res.file_count, res.files_written);
    TEST_ASSERT_TRUE(res.file_count > 3);
    TEST_ASSERT_NOT_NULL(find("E:/Games/Small Game/.xcat-installed"));
    TEST_ASSERT_NULL(find("E:/Games/Small Game/.xcat-journal"));
    /* every file plus the marker */
    TEST_ASSERT_EQUAL_size_t(res.file_count + 1, count_prefix("E:/Games/Small Game/"));
    TEST_ASSERT_TRUE(res.bytes_read >= res.bytes_written);
}

static void test_no_space(void)
{
    fs.free_bytes = 100000;
    TEST_ASSERT_EQUAL_INT(XC_ERR_NO_SPACE, xc_install(&req, &res));
    TEST_ASSERT_TRUE(res.space_needed > res.space_free);
    TEST_ASSERT_EQUAL_UINT64(100000, res.space_free);
    TEST_ASSERT_NULL(find("E:/Games/Small Game")); /* nothing was created */
}

static void test_folder_names_run_out(void)
{
    char path[XC_INSTALL_MAX_PATH];
    (void)create("E:/Games/Small Game", true);
    for (int k = 2; k <= XC_INSTALL_MAX_NUMBERED; k++) {
        (void)snprintf(path, sizeof path, "E:/Games/Small Game (%d)", k);
        (void)create(path, true);
    }
    TEST_ASSERT_EQUAL_INT(XC_ERR_FOLDER, xc_install(&req, &res));
    TEST_ASSERT_EQUAL_STRING("a folder of that name exists", res.other_folder_reason);
}

static void test_cancel_then_resume(void)
{
    cancel_after = 3;
    TEST_ASSERT_EQUAL_INT(XC_ERR_CANCELLED, xc_install(&req, &res));
    TEST_ASSERT_NOT_NULL(find("E:/Games/Small Game/.xcat-journal"));
    TEST_ASSERT_NULL(find("E:/Games/Small Game/.xcat-installed"));
    size_t first = res.files_written;
    TEST_ASSERT_TRUE(first > 0 && first < res.file_count);
    cancel_after = -1;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_install(&req, &res));
    TEST_ASSERT_TRUE(res.resumed);
    TEST_ASSERT_EQUAL_STRING("E:/Games/Small Game", res.folder);
    TEST_ASSERT_EQUAL_size_t(first, res.files_skipped);
    TEST_ASSERT_EQUAL_size_t(res.file_count, res.files_skipped + res.files_written);
    TEST_ASSERT_NOT_NULL(find("E:/Games/Small Game/.xcat-installed"));
    TEST_ASSERT_NULL(find("E:/Games/Small Game (2)"));
}

static void test_missing_done_file_is_rewritten(void)
{
    cancel_after = 3;
    TEST_ASSERT_EQUAL_INT(XC_ERR_CANCELLED, xc_install(&req, &res));
    size_t first = res.files_written;
    /* Remove the first non-empty finished file behind the journal's back. */
    node *j = find("E:/Games/Small Game/.xcat-journal");
    TEST_ASSERT_NOT_NULL(j);
    char *line = strstr((char *)j->data, "\ndone ");
    TEST_ASSERT_NOT_NULL(line);
    char victim[XC_INSTALL_MAX_PATH] = "";
    for (; line != NULL; line = strstr(line + 1, "\ndone ")) {
        char size[32];
        char crc[16];
        char name[256];
        if (sscanf(line + 6, "%31s %15s %255s", size, crc, name) == 3 && strcmp(size, "0") != 0) {
            (void)snprintf(victim, sizeof victim, "E:/Games/Small Game/%s", name);
            break;
        }
    }
    TEST_ASSERT_TRUE(victim[0] != '\0' && strchr(victim, '%') == NULL);
    drop(victim);
    cancel_after = -1;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_install(&req, &res));
    TEST_ASSERT_TRUE(res.resumed);
    TEST_ASSERT_EQUAL_size_t(first - 1, res.files_skipped);
    TEST_ASSERT_NOT_NULL(find(victim));
}

static void test_journal_failure_stops(void)
{
    fs.appends_before_failure = 2; /* the identity lines, one done line, then failure */
    TEST_ASSERT_EQUAL_INT(XC_ERR_IO, xc_install(&req, &res));
    TEST_ASSERT_NULL(find("E:/Games/Small Game/.xcat-installed"));
    fs.appends_before_failure = -1;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_install(&req, &res));
    TEST_ASSERT_TRUE(res.resumed);
    TEST_ASSERT_EQUAL_size_t(1, res.files_skipped);
}

static void test_other_folders_are_never_touched(void)
{
    /* A finished install and a foreign folder keep their contents; the game goes to (3). */
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_install(&req, &res));
    (void)create("E:/Games/Small Game (2)", true);
    (void)create("E:/Games/Small Game (2)/save.dat", false);
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_install(&req, &res));
    TEST_ASSERT_EQUAL_STRING("E:/Games/Small Game (3)", res.folder);
    TEST_ASSERT_EQUAL_STRING("already installed there", res.other_folder_reason);
    TEST_ASSERT_NOT_NULL(find("E:/Games/Small Game (2)/save.dat"));
}

/* Journal lines that start with `prefix` and mention `name` (NULL: any). */
static size_t count_lines(const char *path, const char *prefix, const char *name)
{
    node *n = find(path);
    TEST_ASSERT_NOT_NULL(n);
    size_t count = 0;
    const char *p = (const char *)n->data;
    const char *end = p + n->len;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *stop = nl != NULL ? nl : end;
        size_t n_len = name != NULL ? strlen(name) : 0;
        bool named = name == NULL;
        for (const char *q = p; !named && q + n_len <= stop; q++) {
            named = memcmp(q, name, n_len) == 0;
        }
        if (strncmp(p, prefix, strlen(prefix)) == 0 && named) {
            count++;
        }
        p = nl != NULL ? nl + 1 : end;
    }
    return count;
}

static void test_resume_inside_a_file(void)
{
    req.checkpoint_bytes = 16384;
    cancel_in = "media/intro.xmv"; /* 100,000 bytes: checkpoints at 16 KiB, 32 KiB, ... */
    cancel_in_reports = 3;
    TEST_ASSERT_EQUAL_INT(XC_ERR_CANCELLED, xc_install(&req, &res));
    TEST_ASSERT_TRUE(syncs > 0);
    const char *journal = "E:/Games/Small Game/.xcat-journal";
    size_t parts = count_lines(journal, "part ", "media/intro.xmv");
    TEST_ASSERT_TRUE(parts >= 1);
    TEST_ASSERT_NOT_NULL(strstr((char *)find(journal)->data, "part 16384 "));
    TEST_ASSERT_NOT_NULL(strstr((char *)find(journal)->data, "# xcat-journal v1.1 *"));
    cancel_in = NULL;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_install(&req, &res));
    TEST_ASSERT_TRUE(res.resumed);
    TEST_ASSERT_EQUAL_size_t(1, res.files_continued);
    TEST_ASSERT_EQUAL_UINT64(16384U * parts, res.bytes_continued);
    /* The CRC carried across the checkpoint covers the whole file: the read-back passed. */
    TEST_ASSERT_NOT_NULL(find("E:/Games/Small Game/.xcat-installed"));
}

static void test_checkpoint_ignored_if_file_is_short(void)
{
    req.checkpoint_bytes = 16384;
    cancel_in = "media/intro.xmv";
    cancel_in_reports = 3;
    TEST_ASSERT_EQUAL_INT(XC_ERR_CANCELLED, xc_install(&req, &res));
    node *f = find("E:/Games/Small Game/media/intro.xmv");
    TEST_ASSERT_NOT_NULL(f);
    f->len = 100; /* shorter than any checkpoint, as if the disk lost the tail */
    cancel_in = NULL;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_install(&req, &res));
    TEST_ASSERT_EQUAL_size_t(0, res.files_continued); /* written again from the start */
    TEST_ASSERT_NOT_NULL(find("E:/Games/Small Game/.xcat-installed"));
}

static void test_invalid_arguments(void)
{
    req.buf_size = 1000;
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_install(&req, &res));
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_install(NULL, &res));
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        fixtures_dir = argv[1];
    }
    char path[1024];
    (void)snprintf(path, sizeof path, "%s/small.iso", fixtures_dir);
    if (xc_host_file_open(&image, &image_file, path) != XC_OK) {
        (void)printf("can't open %s\n", path);
        return 1;
    }
    UNITY_BEGIN();
    RUN_TEST(test_fresh_install);
    RUN_TEST(test_no_space);
    RUN_TEST(test_folder_names_run_out);
    RUN_TEST(test_cancel_then_resume);
    RUN_TEST(test_missing_done_file_is_rewritten);
    RUN_TEST(test_journal_failure_stops);
    RUN_TEST(test_other_folders_are_never_touched);
    RUN_TEST(test_resume_inside_a_file);
    RUN_TEST(test_checkpoint_ignored_if_file_is_short);
    RUN_TEST(test_invalid_arguments);
    int rc = UNITY_END();
    setUp(); /* frees the file system */
    xc_host_file_close(&image_file);
    return rc;
}
