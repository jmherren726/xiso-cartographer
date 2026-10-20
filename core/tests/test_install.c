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
#include "xcat/zip.h"

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
    bool track_usage;           /* free space goes down as files grow */
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
    uint64_t used = 0;
    for (size_t i = 0; fs.track_usage && i < fs.count; i++) {
        used += fs.nodes[i].len;
    }
    *bytes = used < fs.free_bytes ? fs.free_bytes - used : 0;
    return XC_OK;
}

static int spool_files_now; /* how many spool files exist */
static int spool_files_peak;

static xc_err f_spool_append(void *ctx, const char *path, const void *data, size_t len)
{
    (void)ctx;
    node *n = find(path);
    if (n == NULL) {
        n = create(path, false);
        spool_files_now++;
        spool_files_peak = spool_files_now > spool_files_peak ? spool_files_now : spool_files_peak;
    }
    add_bytes(n, data, len);
    return XC_OK;
}

static xc_err f_spool_read(void *ctx, const char *path, uint64_t pos, void *out, size_t len)
{
    (void)ctx;
    node *n = find(path);
    TEST_ASSERT_NOT_NULL(n);
    TEST_ASSERT_TRUE(pos <= n->len && len <= n->len - pos);
    memcpy(out, n->data + pos, len);
    return XC_OK;
}

static xc_err f_spool_remove(void *ctx, const char *path)
{
    (void)ctx;
    if (find(path) != NULL) {
        spool_files_now--;
    }
    drop(path);
    return XC_OK;
}

static int spool_syncs;

static xc_err f_spool_sync(void *ctx)
{
    (void)ctx;
    spool_syncs++;
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
    '/',          f_exists,     f_size,         f_read_all,   f_append,
    f_write_all,  f_remove,     f_free,         f_open_sink,  f_close_sink,
    f_open_file,  f_close_file, f_spool_append, f_spool_read, f_spool_remove,
    f_spool_sync, NULL,
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
    spool_files_now = 0;
    spool_files_peak = 0;
    spool_syncs = 0;
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

/* archiveorg.md §8: a listing's size that isn't the source's stops the install at once. */
static void test_expected_size(void)
{
    req.expect_size = image.size + 1U;
    TEST_ASSERT_EQUAL_INT(XC_ERR_SOURCE_SIZE, xc_install(&req, &res));
    TEST_ASSERT_EQUAL_size_t(0, fs.count); /* nothing was created */
    req.expect_size = image.size;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_install(&req, &res));
}

/* The fixture's default.xbe is a pattern, not an XBE: the game installs without a title ID
 * (xbe.md), and the marker has none. */
static void test_title_from_default_xbe(void)
{
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_install(&req, &res));
    TEST_ASSERT_FALSE(res.has_title_id);
    TEST_ASSERT_EQUAL_STRING("default.xbe isn't an XBE", res.title_note);
    node *m = find("E:/Games/Small Game/.xcat-installed");
    TEST_ASSERT_NOT_NULL(m);
    add_bytes(m, "", 1);
    TEST_ASSERT_NULL(strstr((char *)m->data, "title_id="));
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
    TEST_ASSERT_NOT_NULL(strstr((char *)find(journal)->data, "# xcat-journal v1.4 *"));
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

/* --- whole-image hashes (journal.md §7) ----------------------------------------------------- */

static uint8_t want_md5[XC_MD5_BYTES];
static uint8_t want_sha1[XC_SHA1_BYTES];

/* The image's MD5 and SHA-1, the slow obvious way. */
static void hash_image(void)
{
    xc_image_hash *h = NULL;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_image_hash_create(&h));
    for (uint64_t pos = 0; pos < image.size;) {
        size_t n = image.size - pos < sizeof buf ? (size_t)(image.size - pos) : sizeof buf;
        TEST_ASSERT_EQUAL_INT(XC_OK, xc_source_read(&image, pos, buf, n));
        xc_image_hash_update(h, buf, n);
        pos += n;
    }
    xc_image_hash_final(h, want_md5, want_sha1);
    xc_image_hash_free(h);
}

static void expect_image_hash(void)
{
    TEST_ASSERT_NOT_EQUAL_INT(XC_INSTALL_HASH_NONE, res.hash);
    TEST_ASSERT_EQUAL_MEMORY(want_md5, res.md5, XC_MD5_BYTES);
    TEST_ASSERT_EQUAL_MEMORY(want_sha1, res.sha1, XC_SHA1_BYTES);
}

static void test_hash_fresh_install(void)
{
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_install(&req, &res));
    TEST_ASSERT_EQUAL_INT(XC_INSTALL_HASH_COMPUTED, res.hash);
    expect_image_hash();
    TEST_ASSERT_FALSE(res.hash_restarted);
    TEST_ASSERT_TRUE(res.bytes_read >= image.size); /* every byte, the tables twice */
    char md5[2 * XC_MD5_BYTES + 1];
    xc_hex_lower(want_md5, XC_MD5_BYTES, md5);
    node *m = find("E:/Games/Small Game/.xcat-installed");
    TEST_ASSERT_NOT_NULL(m);
    add_bytes(m, "", 1); /* NUL-terminate for strstr */
    TEST_ASSERT_NOT_NULL(strstr((char *)m->data, md5));
    TEST_ASSERT_NOT_NULL(strstr((char *)m->data, "verified=crc32\n"));
}

static void test_hash_expected_values(void)
{
    req.expect_md5 = want_md5;
    req.expect_sha1 = want_sha1;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_install(&req, &res));
    TEST_ASSERT_EQUAL_INT(XC_INSTALL_HASH_MATCHED, res.hash);
    node *m = find("E:/Games/Small Game/.xcat-installed");
    TEST_ASSERT_NOT_NULL(m);
    add_bytes(m, "", 1);
    TEST_ASSERT_NOT_NULL(strstr((char *)m->data, "verified=crc32,image-hash\n"));
}

static void test_hash_mismatch_fails(void)
{
    uint8_t wrong[XC_SHA1_BYTES];
    memcpy(wrong, want_sha1, sizeof wrong);
    wrong[7] ^= 1U;
    req.expect_md5 = want_md5;
    req.expect_sha1 = wrong;
    TEST_ASSERT_EQUAL_INT(XC_ERR_IMAGE_HASH, xc_install(&req, &res));
    expect_image_hash(); /* the computed values are reported */
    TEST_ASSERT_NULL(find("E:/Games/Small Game/.xcat-installed"));
    TEST_ASSERT_NOT_NULL(find("E:/Games/Small Game/.xcat-journal"));
}

static void test_hash_survives_resume(void)
{
    req.checkpoint_bytes = 16384; /* also the distance between hash lines */
    cancel_in = "media/intro.xmv";
    cancel_in_reports = 3;
    TEST_ASSERT_EQUAL_INT(XC_ERR_CANCELLED, xc_install(&req, &res));
    const char *journal = "E:/Games/Small Game/.xcat-journal";
    TEST_ASSERT_TRUE(count_lines(journal, "hash ", NULL) >= 1);
    cancel_in = NULL;
    uint64_t first_read = res.bytes_read;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_install(&req, &res));
    TEST_ASSERT_TRUE(res.resumed);
    TEST_ASSERT_FALSE(res.hash_restarted);
    expect_image_hash();
    /* The resume read on from the saved state, not the whole image again. */
    TEST_ASSERT_TRUE(res.bytes_read < image.size);
    TEST_ASSERT_TRUE(first_read + res.bytes_read < 2 * image.size);
}

static void test_hash_restarts_when_needed(void)
{
    /* A v1.1-style journal without hash lines: the resume hashes from byte 0 again. */
    cancel_after = 3;
    TEST_ASSERT_EQUAL_INT(XC_ERR_CANCELLED, xc_install(&req, &res));
    TEST_ASSERT_EQUAL_size_t(0, count_lines("E:/Games/Small Game/.xcat-journal", "hash ", NULL));
    cancel_after = -1;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_install(&req, &res));
    TEST_ASSERT_TRUE(res.resumed);
    TEST_ASSERT_TRUE(res.hash_restarted);
    expect_image_hash();
}

static void test_hash_turned_off(void)
{
    req.no_hash = true;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_install(&req, &res));
    TEST_ASSERT_EQUAL_INT(XC_INSTALL_HASH_NONE, res.hash);
    TEST_ASSERT_EQUAL_STRING("not asked for", res.hash_note);
}

static void test_invalid_arguments(void)
{
    req.buf_size = 1000;
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_install(&req, &res));
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_install(NULL, &res));
}

/* --- zips (docs/formats/zip.md, stream-install.md) ------------------------------------- */

static xc_host_file zip_file;
static xc_source zip_src;
static bool zip_open;

/* Points the request at fixtures_dir/<name>.zip. */
static void use_zip(const char *name)
{
    char path[1024];
    if (zip_open) {
        xc_host_file_close(&zip_file);
    }
    (void)snprintf(path, sizeof path, "%s/%s.zip", fixtures_dir, name);
    TEST_ASSERT_EQUAL_INT_MESSAGE(XC_OK, xc_host_file_open(&zip_src, &zip_file, path), path);
    zip_open = true;
    req.src = &zip_src;
    req.zip = true;
    req.source_name = name;
}

static void expect_finished(void)
{
    TEST_ASSERT_NOT_NULL(find("E:/Games/Small Game/.xcat-installed"));
    TEST_ASSERT_NULL(find("E:/Games/Small Game/.xcat-journal"));
    TEST_ASSERT_EQUAL_INT(0, spool_files_now);
    TEST_ASSERT_EQUAL_size_t(res.file_count + 1, count_prefix("E:/Games/Small Game/"));
}

static void test_zip_stored_member(void)
{
    use_zip("small-stored");
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_install(&req, &res));
    TEST_ASSERT_EQUAL_UINT16(XC_ZIP_STORED, res.zip_method);
    TEST_ASSERT_EQUAL_STRING("small.iso", res.zip_member);
    TEST_ASSERT_EQUAL_UINT(0, res.passes); /* read with Range, like a bare image */
    TEST_ASSERT_EQUAL_INT(XC_INSTALL_HASH_COMPUTED, res.hash);
    expect_finished();
}

static void test_zip_deflate_tables_first(void)
{
    use_zip("small-deflate");
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_install(&req, &res));
    TEST_ASSERT_EQUAL_UINT16(XC_ZIP_DEFLATE, res.zip_method);
    TEST_ASSERT_EQUAL_UINT(1, res.passes);
    TEST_ASSERT_EQUAL_size_t(0, res.files_from_spool);
    TEST_ASSERT_EQUAL_size_t(res.file_count, res.files_written);
    TEST_ASSERT_EQUAL_INT(XC_INSTALL_HASH_NONE, res.hash);
    expect_finished();
}

static void test_zip_deflate_late_tables(void)
{
    use_zip("late-deflate");
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_install(&req, &res));
    TEST_ASSERT_EQUAL_UINT(1, res.passes);
    TEST_ASSERT_TRUE(res.files_from_spool > 0);
    TEST_ASSERT_TRUE(res.spool_peak > 0);
    TEST_ASSERT_TRUE(spool_files_peak > 0);
    expect_finished();
}

/* The files, the reserve, the checkpoints and states (16 x 41 KiB and 2 x 64 KiB here), and
   256 KiB to spare. */
#define TIGHT_DISK (197333U + 65536U + 16U * 41204U + 2U * 65536U + 262144U)

static void test_zip_tight_space_second_pass(void)
{
    use_zip("late-deflate");
    req.cluster_size = 0;
    req.checkpoint_bytes = 65536; /* spool chunks and zip checkpoints every 64 KiB */
    req.spool_reserve = 65536;
    fs.track_usage = true;
    fs.free_bytes = TIGHT_DISK;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_install(&req, &res));
    TEST_ASSERT_TRUE(res.passes >= 2);
    TEST_ASSERT_TRUE(res.reread_bytes > 0);
    expect_finished();
}

static void test_zip_cancel_then_resume(void)
{
    use_zip("small-deflate");
    cancel_after = 2;
    TEST_ASSERT_EQUAL_INT(XC_ERR_CANCELLED, xc_install(&req, &res));
    node *j = find("E:/Games/Small Game/.xcat-journal");
    TEST_ASSERT_NOT_NULL(j);
    TEST_ASSERT_NOT_NULL(strstr((const char *)j->data, "zip 786432 "));
    size_t first = res.files_written;
    TEST_ASSERT_TRUE(first > 0 && first < res.file_count);
    cancel_after = -1;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_install(&req, &res));
    TEST_ASSERT_TRUE(res.resumed);
    TEST_ASSERT_EQUAL_size_t(first, res.files_skipped);
    TEST_ASSERT_EQUAL_size_t(res.file_count, res.files_skipped + res.files_written);
    expect_finished();
}

static void test_zip_other_member_is_another_image(void)
{
    use_zip("small-deflate");
    cancel_after = 2;
    TEST_ASSERT_EQUAL_INT(XC_ERR_CANCELLED, xc_install(&req, &res));
    cancel_after = -1;
    use_zip("late-deflate"); /* same files, different member: the journal doesn't match */
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_install(&req, &res));
    TEST_ASSERT_FALSE(res.resumed);
    TEST_ASSERT_EQUAL_STRING("E:/Games/Small Game (2)", res.folder);
    TEST_ASSERT_NOT_NULL(find("E:/Games/Small Game/.xcat-journal"));
}

static void test_zip_without_spool_io(void)
{
    static xc_install_io no_spool;
    no_spool = memio;
    no_spool.spool_append = NULL;
    use_zip("small-deflate");
    req.io = &no_spool;
    TEST_ASSERT_EQUAL_INT(XC_ERR_UNSUPPORTED, xc_install(&req, &res));
}

static void test_zip_damaged_member(void)
{
    /* a copy of the zip with one byte of the compressed image changed */
    char path[1024];
    (void)snprintf(path, sizeof path, "%s/small-deflate.zip", fixtures_dir);
    xc_host_file hf = {NULL};
    xc_source src;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_host_file_open(&src, &hf, path));
    size_t len = (size_t)src.size;
    uint8_t *copy = malloc(len);
    TEST_ASSERT_NOT_NULL(copy);
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_source_read(&src, 0, copy, len));
    xc_host_file_close(&hf);
    copy[len / 2] ^= 0x10U;
    xc_mem_source mem;
    xc_source_memory(&zip_src, &mem, copy, len);
    req.src = &zip_src;
    req.zip = true;
    xc_err err = xc_install(&req, &res);
    TEST_ASSERT_TRUE(err == XC_ERR_ZIP_CRC || err == XC_ERR_CORRUPT);
    TEST_ASSERT_NULL(find("E:/Games/Small Game/.xcat-installed"));
    free(copy);
}

/* --- resuming a Deflate zip in the middle of the stream (stream-install.md §7) ---------- */

#define ZDIR "E:/Games/Small Game/"

/* A fresh file system and request for `zip`, states saved every 64 KiB. */
static void zip_attempt_setup(const char *zip)
{
    setUp();
    use_zip(zip);
    req.checkpoint_bytes = 65536;
}

/*
 * Cuts the install at its `appends`-th durable append (a journal line, a checkpoint), as a
 * power cut there would, then resumes; the install must finish.
 */
static void cut_and_resume(const char *zip, int appends, uint64_t *resumed_at, size_t *continued)
{
    fs.appends_before_failure = appends;
    xc_err err = xc_install(&req, &res);
    fs.appends_before_failure = -1;
    if (err == XC_OK) {
        expect_finished(); /* it finished first */
        return;
    }
    TEST_ASSERT_EQUAL_INT(XC_ERR_IO, err);
    err = xc_install(&req, &res);
    char msg[512];
    (void)snprintf(msg, sizeof msg, "%s, cut at append %d: %s", zip, appends, res.current_file);
    TEST_ASSERT_EQUAL_INT_MESSAGE(XC_OK, err, msg);
    TEST_ASSERT_TRUE(res.resumed);
    expect_finished();
    *resumed_at = *resumed_at > res.zip_resumed_at ? *resumed_at : res.zip_resumed_at;
    *continued += res.files_continued;
}

static void test_zip_resume_mid_stream(void)
{
    static const char *zips[] = {"late-deflate", "small-deflate", "late-stored"};
    for (size_t z = 0; z < sizeof zips / sizeof zips[0]; z++) {
        uint64_t resumed_at = 0;
        size_t continued = 0;
        for (int appends = 1; appends < 60; appends += 2) {
            zip_attempt_setup(zips[z]);
            cut_and_resume(zips[z], appends, &resumed_at, &continued);
        }
        if (z < 2) {
            TEST_ASSERT_TRUE_MESSAGE(resumed_at > 0, zips[z]); /* not from the start again */
        }
    }
}

static void test_zip_state_files(void)
{
    zip_attempt_setup("late-deflate");
    fs.appends_before_failure = 20;
    TEST_ASSERT_EQUAL_INT(XC_ERR_IO, xc_install(&req, &res));
    fs.appends_before_failure = -1;
    const char *journal = ZDIR ".xcat-journal";
    TEST_ASSERT_TRUE(count_lines(journal, "state ", NULL) > 0);
    TEST_ASSERT_TRUE(spool_syncs > 0);
    node *ck = find(ZDIR XC_INSTALL_ZIP_CHECKPOINTS);
    TEST_ASSERT_NOT_NULL(ck);
    TEST_ASSERT_EQUAL_size_t(0, ck->len % xc_zip_checkpoint_size(XC_ZIP_DEFLATE));
    TEST_ASSERT_TRUE(find(ZDIR ".xcat-state-0") != NULL || find(ZDIR ".xcat-state-1") != NULL);
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_install(&req, &res));
    TEST_ASSERT_TRUE(res.zip_resumed_at > 0);
    expect_finished(); /* the state and checkpoint files are gone too */
}

static void test_zip_resume_tight_space(void)
{
    uint64_t resumed_at = 0;
    size_t continued = 0;
    unsigned most_passes = 0;
    for (int appends = 1; appends < 120; appends += 3) {
        zip_attempt_setup("late-deflate");
        req.cluster_size = 0;
        req.spool_reserve = 65536; /* as in test_zip_tight_space_second_pass */
        fs.track_usage = true;
        fs.free_bytes = TIGHT_DISK;
        cut_and_resume("late-deflate", appends, &resumed_at, &continued);
        most_passes = res.passes > most_passes ? res.passes : most_passes;
    }
    TEST_ASSERT_TRUE(resumed_at > 0);
    TEST_ASSERT_TRUE(most_passes >= 2);
}

/* A cancelled install, then `damage` applied to its folder, then a resume that finishes. */
static void resume_after(void (*damage)(void))
{
    zip_attempt_setup("late-deflate");
    fs.appends_before_failure = 20;
    TEST_ASSERT_EQUAL_INT(XC_ERR_IO, xc_install(&req, &res));
    fs.appends_before_failure = -1;
    TEST_ASSERT_TRUE(count_prefix(ZDIR ".xcat-spool-") > 0);
    damage();
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_install(&req, &res));
    TEST_ASSERT_TRUE(res.resumed);
    expect_finished();
}

static void flip_state_files(void)
{
    for (int k = 0; k < 2; k++) {
        node *n = find(k == 0 ? ZDIR ".xcat-state-0" : ZDIR ".xcat-state-1");
        if (n != NULL) {
            n->data[n->len / 2] ^= 0x01U;
        }
    }
}

static void tear_checkpoints(void)
{
    node *n = find(ZDIR XC_INSTALL_ZIP_CHECKPOINTS);
    TEST_ASSERT_NOT_NULL(n);
    n->len -= 1000; /* a power cut during the last append */
}

static void drop_checkpoints(void)
{
    drop(ZDIR XC_INSTALL_ZIP_CHECKPOINTS);
}

static void test_zip_damaged_state_starts_again(void)
{
    resume_after(flip_state_files);
    TEST_ASSERT_EQUAL_UINT64(0, res.zip_resumed_at);
}

static void test_zip_torn_checkpoints(void)
{
    resume_after(tear_checkpoints);
    resume_after(drop_checkpoints);
    TEST_ASSERT_EQUAL_UINT64(0, res.zip_resumed_at); /* no checkpoint at the state's offset */
}

static void test_zip_stale_spool_without_states(void)
{
    /* without spool_sync no state is saved; the spool files left are deleted on resume */
    static xc_install_io no_sync;
    no_sync = memio;
    no_sync.spool_sync = NULL;
    zip_attempt_setup("late-deflate");
    req.io = &no_sync;
    fs.appends_before_failure = 3; /* the start, then two files out of the spool */
    TEST_ASSERT_EQUAL_INT(XC_ERR_IO, xc_install(&req, &res));
    fs.appends_before_failure = -1;
    TEST_ASSERT_TRUE(count_prefix(ZDIR ".xcat-spool-") > 0);
    TEST_ASSERT_EQUAL_size_t(0, count_lines(ZDIR ".xcat-journal", "state ", NULL));
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_install(&req, &res));
    TEST_ASSERT_EQUAL_UINT64(0, res.zip_resumed_at);
    expect_finished();
}

static void test_zip_not_a_zip(void)
{
    req.zip = true; /* small.iso is no zip */
    TEST_ASSERT_EQUAL_INT(XC_ERR_NOT_ZIP, xc_install(&req, &res));
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
    RUN_TEST(test_expected_size);
    RUN_TEST(test_title_from_default_xbe);
    RUN_TEST(test_no_space);
    RUN_TEST(test_folder_names_run_out);
    RUN_TEST(test_cancel_then_resume);
    RUN_TEST(test_missing_done_file_is_rewritten);
    RUN_TEST(test_journal_failure_stops);
    RUN_TEST(test_other_folders_are_never_touched);
    RUN_TEST(test_resume_inside_a_file);
    RUN_TEST(test_checkpoint_ignored_if_file_is_short);
    hash_image();
    RUN_TEST(test_hash_fresh_install);
    RUN_TEST(test_hash_expected_values);
    RUN_TEST(test_hash_mismatch_fails);
    RUN_TEST(test_hash_survives_resume);
    RUN_TEST(test_hash_restarts_when_needed);
    RUN_TEST(test_hash_turned_off);
    RUN_TEST(test_invalid_arguments);
    RUN_TEST(test_zip_stored_member);
    RUN_TEST(test_zip_deflate_tables_first);
    RUN_TEST(test_zip_deflate_late_tables);
    RUN_TEST(test_zip_tight_space_second_pass);
    RUN_TEST(test_zip_cancel_then_resume);
    RUN_TEST(test_zip_other_member_is_another_image);
    RUN_TEST(test_zip_without_spool_io);
    RUN_TEST(test_zip_damaged_member);
    RUN_TEST(test_zip_resume_mid_stream);
    RUN_TEST(test_zip_state_files);
    RUN_TEST(test_zip_resume_tight_space);
    RUN_TEST(test_zip_damaged_state_starts_again);
    RUN_TEST(test_zip_torn_checkpoints);
    RUN_TEST(test_zip_stale_spool_without_states);
    RUN_TEST(test_zip_not_a_zip);
    int rc = UNITY_END();
    if (zip_open) {
        xc_host_file_close(&zip_file);
    }
    setUp(); /* frees the file system */
    xc_host_file_close(&image_file);
    return rc;
}
