/*
 * xiso-cartographer XBE, milestone M2: install an image that is already on the hard disk.
 *
 * Looks for images in E:\xcat\isos\. If there are none, it first copies the bundled
 * synthetic test image there (D:\samples\*.iso: dummy data from tools/fixtures, never a
 * game). Each image is checked (FATX rules, ADR 0008; free space), extracted to
 * E:\Games\<title>, verified by reading every installed file back and comparing it with
 * the image, and marked with .xcat-installed. The real UI replaces this in M8.
 */
#include <hal/debug.h>
#include <hal/video.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "xcat/extract.h"
#include "xcat/fatx.h"
#include "xcat/nxdk.h"
#include "xcat/version.h"
#include "xcat/xdvdfs.h"

#define ISO_DIR "E:\\xcat\\isos"
#define SAMPLE_DIR "D:\\samples"
#define GAMES_DIR "E:\\Games"
#define IO_BUF_SIZE ((size_t)1024 * 1024)
#define CLUSTER 16384U /* FATX cluster size on a retail E: partition; for the space estimate */
#define MAX_IMAGES 16
#define MAX_PROBLEMS 8

static uint8_t *io_buf;
static uint8_t *cmp_buf;

static void halt(void)
{
    for (;;) {
        Sleep(1000);
    }
}

/* If E:\xcat\isos has no images, copies the bundled samples there. */
static void seed_samples(void)
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(ISO_DIR "\\*.iso", &fd);
    if (h != INVALID_HANDLE_VALUE) {
        FindClose(h);
        return;
    }
    h = FindFirstFileA(SAMPLE_DIR "\\*.iso", &fd);
    if (h == INVALID_HANDLE_VALUE) {
        debugPrint("No images in " ISO_DIR " and no bundled samples.\n");
        return;
    }
    do {
        char from[XC_NX_PATH_MAX];
        char to[XC_NX_PATH_MAX];
        snprintf(from, sizeof from, SAMPLE_DIR "\\%s", fd.cFileName);
        snprintf(to, sizeof to, ISO_DIR "\\%s", fd.cFileName);
        debugPrint("Copying bundled test image %s to the HDD\n", fd.cFileName);
        if (!CopyFileA(from, to, TRUE)) {
            debugPrint("  copy failed (error %lu)\n", (unsigned long)GetLastError());
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

/* True if the `n` bytes before s[len] equal `suffix`, ignoring ASCII case. */
static bool ends_with_ci(const char *s, size_t len, const char *suffix)
{
    size_t n = strlen(suffix);
    if (len < n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char a = s[len - n + i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

/* "Halo (USA).xiso.iso" -> "Halo (USA)" */
static void title_from_file(const char *file, char *out, size_t size)
{
    size_t len = strlen(file);
    if (ends_with_ci(file, len, ".iso")) {
        len -= 4;
    }
    if (ends_with_ci(file, len, ".xiso")) {
        len -= 5;
    }
    if (len >= size) {
        len = size - 1;
    }
    memcpy(out, file, len);
    out[len] = '\0';
}

typedef struct progress {
    int last_tenth;
} progress;

static bool on_progress(void *ctx, const xc_extract_progress *p)
{
    progress *pr = ctx;
    int pct = p->bytes_total > 0 ? (int)(p->bytes_done * 100U / p->bytes_total) : 100;
    if (pct / 10 > pr->last_tenth) {
        pr->last_tenth = pct / 10;
        debugPrint(" %d%%", pct);
    }
    return true;
}

/* Reads every installed file back and compares it with the image. */
static size_t verify(const xc_source *img, const xc_xdvdfs_listing *l, const char *dir)
{
    size_t bad = 0;
    for (size_t i = 0; i < l->file_count; i++) {
        const xc_xdvdfs_entry *e = &l->files[i];
        char path[XC_NX_PATH_MAX];
        snprintf(path, sizeof path, "%s\\%s", dir, e->path);
        for (char *c = path; *c != '\0'; c++) {
            if (*c == '/') {
                *c = '\\';
            }
        }
        xc_nx_file f = {NULL};
        xc_source out;
        if (xc_nx_file_open(&out, &f, path) != XC_OK || out.size != e->size) {
            debugPrint("  MISSING or wrong size: %s\n", e->path);
            xc_nx_file_close(&f);
            bad++;
            continue;
        }
        for (uint64_t pos = 0; pos < e->size;) {
            size_t n = e->size - pos < IO_BUF_SIZE ? (size_t)(e->size - pos) : IO_BUF_SIZE;
            if (xc_source_read(img, e->offset + pos, io_buf, n) != XC_OK ||
                xc_source_read(&out, pos, cmp_buf, n) != XC_OK || memcmp(io_buf, cmp_buf, n) != 0) {
                debugPrint("  CONTENT MISMATCH: %s\n", e->path);
                bad++;
                break;
            }
            pos += n;
        }
        xc_nx_file_close(&f);
    }
    return bad;
}

static void write_marker(const char *dir, const char *image, const xc_xdvdfs_listing *l)
{
    char path[XC_NX_PATH_MAX];
    snprintf(path, sizeof path, "%s\\.xcat-installed", dir);
    char text[512];
    int n = snprintf(text, sizeof text,
                     "xiso-cartographer %s\nimage=%s\nfiles=%u\nbytes=%llu\nverified=yes\n",
                     xc_version_string(), image, (unsigned)l->file_count,
                     (unsigned long long)l->total_file_bytes);
    HANDLE h =
        CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD put = 0;
    if (h == INVALID_HANDLE_VALUE || n <= 0 || !WriteFile(h, text, (DWORD)n, &put, NULL)) {
        debugPrint("  warning: could not write .xcat-installed\n");
    }
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
    }
}

static const char *problem_text(const xc_fatx_problem *p)
{
    switch (p->kind) {
    case XC_FATX_PROBLEM_NAME:
        return p->name == XC_FATX_NAME_TOO_LONG ? "name over 42 characters" : "invalid name";
    case XC_FATX_PROBLEM_PATH_TOO_LONG:
        return "path over 250 characters";
    case XC_FATX_PROBLEM_TOO_MANY_ENTRIES:
        return "over 4096 entries in one folder";
    }
    return "problem";
}

/* Returns true if the image was installed and verified. */
static bool install(const char *file)
{
    char image[XC_NX_PATH_MAX];
    snprintf(image, sizeof image, ISO_DIR "\\%s", file);
    debugPrint("\n%s\n", file);

    xc_nx_file f = {NULL};
    xc_source src;
    xc_xdvdfs_volume vol;
    xc_xdvdfs_listing l;
    memset(&l, 0, sizeof l);
    xc_err err = xc_nx_file_open(&src, &f, image);
    if (err == XC_OK) {
        err = xc_xdvdfs_open(&src, &vol);
    }
    if (err == XC_OK) {
        err = xc_xdvdfs_list(&src, &vol, NULL, &l);
    }
    if (err != XC_OK) {
        debugPrint("  can't read image: %s\n", xc_err_str(err));
        xc_nx_file_close(&f);
        return false;
    }
    debugPrint("  %u files, %u folders, %llu KB\n", (unsigned)l.file_count, (unsigned)l.dir_count,
               (unsigned long long)(l.total_file_bytes / 1024U));

    /* Install folder: from the file name, numbered if already taken (ADR 0008). */
    char title[128];
    title_from_file(file, title, sizeof title);
    char base[XC_FATX_MAX_NAME + 1];
    char folder[XC_FATX_MAX_NAME + 1];
    xc_fatx_title_name(title, base, XC_FATX_MAX_NAME);
    memcpy(folder, base, strlen(base) + 1);
    char dir[XC_NX_PATH_MAX];
    snprintf(dir, sizeof dir, GAMES_DIR "\\%s", folder);
    for (unsigned k = 2; xc_nx_exists(dir) && k < 100; k++) {
        xc_fatx_numbered_name(base, k, folder);
        snprintf(dir, sizeof dir, GAMES_DIR "\\%s", folder);
    }

    bool ok = false;
    xc_fatx_problem problems[MAX_PROBLEMS];
    size_t count = 0;
    err = xc_fatx_check_listing(&l, strlen("\\Games\\") + strlen(folder), problems, MAX_PROBLEMS,
                                &count);
    if (err != XC_OK || count > 0) {
        debugPrint("  NOT INSTALLED: %s\n",
                   err != XC_OK ? xc_err_str(err) : xc_err_str(XC_ERR_NOT_INSTALLABLE));
        for (size_t i = 0; i < count && i < MAX_PROBLEMS; i++) {
            debugPrint("   %.48s: %s\n", problems[i].path, problem_text(&problems[i]));
        }
        goto done;
    }

    /* Free space: every file rounded up to whole clusters, plus a cluster per folder. */
    uint64_t need = (uint64_t)(l.dir_count + 2U) * CLUSTER;
    for (size_t i = 0; i < l.file_count; i++) {
        need += ((uint64_t)l.files[i].size + CLUSTER - 1U) / CLUSTER * CLUSTER;
    }
    uint64_t free_bytes = 0;
    if (xc_nx_free_space("E:\\", &free_bytes) != XC_OK || free_bytes < need) {
        debugPrint("  NOT INSTALLED: needs %llu KB, %llu KB free\n",
                   (unsigned long long)(need / 1024U), (unsigned long long)(free_bytes / 1024U));
        goto done;
    }

    debugPrint("  -> %s\n  extracting:", dir);
    xc_sink sink;
    xc_nx_dir_sink ds;
    err = xc_nx_dir_sink_open(&sink, &ds, dir);
    xc_extract_stats st;
    memset(&st, 0, sizeof st);
    DWORD start = GetTickCount();
    if (err == XC_OK) {
        progress pr = {-1};
        xc_extract_opts opts = {io_buf, IO_BUF_SIZE, XC_EXTRACT_DEFAULT_GAP, on_progress, &pr, NULL,
                                NULL,   NULL};
        err = xc_extract(&src, &l, &sink, &opts, &st);
        xc_nx_dir_sink_close(&ds);
    }
    DWORD ms = GetTickCount() - start;
    if (err != XC_OK) {
        debugPrint("\n  FAILED: %s\n", xc_err_str(err));
        goto done;
    }
    debugPrint("\n  wrote %u files in %lu ms", (unsigned)st.files_written, (unsigned long)ms);
    if (ms > 0) {
        debugPrint(" (%llu KB/s)", (unsigned long long)(st.bytes_written / ms * 1000U / 1024U));
    }
    debugPrint("\n  verifying...\n");
    size_t bad = verify(&src, &l, dir);
    if (bad > 0) {
        debugPrint("  VERIFY FAILED: %u files differ\n", (unsigned)bad);
        goto done;
    }
    write_marker(dir, file, &l);
    debugPrint("  OK: all %u files match the image\n", (unsigned)l.file_count);
    ok = true;

done:
    xc_xdvdfs_listing_free(&l);
    xc_nx_file_close(&f);
    return ok;
}

int main(void)
{
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    debugPrint("xiso-cartographer %s - local install (M2)\n", xc_version_string());

    io_buf = malloc(IO_BUF_SIZE);
    cmp_buf = malloc(IO_BUF_SIZE);
    if (io_buf == NULL || cmp_buf == NULL || !xc_nx_mount_e()) {
        debugPrint("Startup failed (memory or E: drive)\n");
        halt();
    }
    if (!xc_nx_mkdirs(ISO_DIR) || !xc_nx_mkdirs(GAMES_DIR)) {
        debugPrint("Could not create " ISO_DIR " or " GAMES_DIR "\n");
        halt();
    }
    seed_samples();

    char names[MAX_IMAGES][XC_NX_PATH_MAX];
    int count = 0;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(ISO_DIR "\\*.iso", &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && count < MAX_IMAGES) {
                snprintf(names[count++], XC_NX_PATH_MAX, "%s", fd.cFileName);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (count == 0) {
        debugPrint("Nothing to install. Put .iso files in " ISO_DIR "\n");
        halt();
    }
    int installed = 0;
    for (int i = 0; i < count; i++) {
        installed += install(names[i]) ? 1 : 0;
    }
    debugPrint("\nDone: %d of %d image(s) installed and verified.\n", installed, count);
    halt();
}
