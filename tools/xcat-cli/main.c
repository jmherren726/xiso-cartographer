/*
 * xcat-cli: run libxcat on a PC.
 *
 *   xcat-cli list <image>               print the layout and every file (offset order)
 *   xcat-cli extract <image> <out-dir>  extract the image into out-dir
 *   xcat-cli check <image> [title]      can it be installed on the Xbox HDD? (ADR 0008)
 *   xcat-cli layout <image>             where directory tables sit vs. file data (ADR 0009)
 *
 * Exit status: 0 success, 1 failure, 2 usage error, 3 not installable (check).
 */
#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xcat/extract.h"
#include "xcat/fatx.h"
#include "xcat/host.h"
#include "xcat/version.h"
#include "xcat/xdvdfs.h"

#define IO_BUFFER_SIZE ((size_t)1024 * 1024)

static int usage(void)
{
    (void)fprintf(stderr,
                  "xcat-cli %s\n"
                  "usage: xcat-cli list <image>\n"
                  "       xcat-cli extract <image> <out-dir>\n"
                  "       xcat-cli check <image> [title]\n"
                  "       xcat-cli layout <image>\n",
                  xc_version_string());
    return 2;
}

static int fail(const char *what, xc_err err)
{
    (void)fprintf(stderr, "xcat-cli: %s: %s\n", what, xc_err_str(err));
    return 1;
}

static const char *layout_name(xc_xdvdfs_layout l)
{
    switch (l) {
    case XC_XDVDFS_XISO:
        return "xiso";
    case XC_XDVDFS_XGD1:
        return "xgd1";
    case XC_XDVDFS_XGD2:
        return "xgd2";
    case XC_XDVDFS_XGD3:
        return "xgd3";
    }
    return "?";
}

/* Opens the image and lists it; on success the caller closes `hf` and frees `l`. */
static int load(const char *image, xc_source *src, xc_host_file *hf, xc_xdvdfs_volume *vol,
                xc_xdvdfs_listing *l)
{
    xc_err err = xc_host_file_open(src, hf, image);
    if (err != XC_OK) {
        return fail(image, err);
    }
    err = xc_xdvdfs_open(src, vol);
    if (err == XC_OK) {
        err = xc_xdvdfs_list(src, vol, NULL, l);
    }
    if (err != XC_OK) {
        xc_host_file_close(hf);
        return fail(image, err);
    }
    return 0;
}

static int cmd_list(const char *image)
{
    xc_source src;
    xc_host_file hf = {NULL};
    xc_xdvdfs_volume vol;
    xc_xdvdfs_listing l;
    int rc = load(image, &src, &hf, &vol, &l);
    if (rc != 0) {
        return rc;
    }
    (void)printf("layout %s, partition at %" PRIu64 ", %zu files, %zu dirs, %" PRIu64 " bytes\n",
                 layout_name(vol.layout), vol.partition_offset, l.file_count, l.dir_count,
                 l.total_file_bytes);
    for (size_t i = 0; i < l.file_count; i++) {
        (void)printf("%12" PRIu64 " %10" PRIu32 "  %s\n", l.files[i].offset, l.files[i].size,
                     l.files[i].path);
    }
    xc_xdvdfs_listing_free(&l);
    xc_host_file_close(&hf);
    return 0;
}

static int cmd_extract(const char *image, const char *out)
{
    xc_source src;
    xc_host_file hf = {NULL};
    xc_xdvdfs_volume vol;
    xc_xdvdfs_listing l;
    int rc = load(image, &src, &hf, &vol, &l);
    if (rc != 0) {
        return rc;
    }
    uint8_t *buf = malloc(IO_BUFFER_SIZE);
    xc_sink sink;
    xc_host_dir_sink ds;
    xc_err err = buf != NULL ? xc_host_dir_sink_open(&sink, &ds, out) : XC_ERR_NOMEM;
    xc_extract_stats st;
    memset(&st, 0, sizeof st);
    if (err == XC_OK) {
        xc_extract_opts opts = {buf, IO_BUFFER_SIZE, XC_EXTRACT_DEFAULT_GAP, NULL, NULL};
        err = xc_extract(&src, &l, &sink, &opts, &st);
        xc_host_dir_sink_close(&ds);
    }
    free(buf);
    xc_xdvdfs_listing_free(&l);
    xc_host_file_close(&hf);
    if (err != XC_OK) {
        return fail(out, err);
    }
    (void)printf("extracted %zu files, %zu dirs, %" PRIu64 " bytes (%" PRIu64
                 " read in %zu spans)\n",
                 st.files_written, st.dirs_created, st.bytes_written, st.bytes_read, st.spans);
    return 0;
}

static bool ends_with_ci(const char *s, size_t len, const char *suffix)
{
    size_t n = strlen(suffix);
    if (len < n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (tolower((unsigned char)s[len - n + i]) != tolower((unsigned char)suffix[i])) {
            return false;
        }
    }
    return true;
}

/* "dir/Halo (USA).xiso.iso" -> "Halo (USA)" (at most `size` - 1 bytes). */
static void title_from_path(const char *path, char *out, size_t size)
{
    const char *base = path;
    for (const char *p = path; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    size_t len = strlen(base);
    if (ends_with_ci(base, len, ".iso")) {
        len -= 4;
    }
    if (ends_with_ci(base, len, ".xiso")) {
        len -= 5;
    }
    if (len >= size) {
        len = size - 1;
    }
    memcpy(out, base, len);
    out[len] = '\0';
}

static const char *name_problem(xc_fatx_name_check c)
{
    switch (c) {
    case XC_FATX_NAME_EMPTY:
        return "empty name";
    case XC_FATX_NAME_TOO_LONG:
        return "name longer than 42 characters";
    case XC_FATX_NAME_BAD_BYTE:
        return "character not allowed on FATX";
    case XC_FATX_NAME_DOTS:
        return "name is . or ..";
    case XC_FATX_NAME_OK:
        break;
    }
    return "invalid name";
}

#define MAX_REPORTED 20

static int cmd_check(const char *image, const char *title)
{
    xc_source src;
    xc_host_file hf = {NULL};
    xc_xdvdfs_volume vol;
    xc_xdvdfs_listing l;
    int rc = load(image, &src, &hf, &vol, &l);
    if (rc != 0) {
        return rc;
    }
    char from_path[256];
    if (title == NULL) {
        title_from_path(image, from_path, sizeof from_path);
        title = from_path;
    }
    char folder[XC_FATX_MAX_NAME + 1];
    size_t folder_len = xc_fatx_title_name(title, folder, XC_FATX_MAX_NAME);
    size_t install_dir_len = strlen("\\Games\\") + folder_len;
    xc_fatx_problem problems[MAX_REPORTED];
    size_t count = 0;
    xc_err err = xc_fatx_check_listing(&l, install_dir_len, problems, MAX_REPORTED, &count);
    if (err != XC_OK) {
        xc_xdvdfs_listing_free(&l);
        xc_host_file_close(&hf);
        return fail(image, err);
    }
    (void)printf("install folder: E:\\Games\\%s\n", folder);
    for (size_t i = 0; i < count && i < MAX_REPORTED; i++) {
        const xc_fatx_problem *p = &problems[i];
        switch (p->kind) {
        case XC_FATX_PROBLEM_NAME:
            (void)printf("  %s: %s\n", p->path, name_problem(p->name));
            break;
        case XC_FATX_PROBLEM_PATH_TOO_LONG:
            (void)printf("  %s: path would be %zu characters (limit %d)\n", p->path, p->value,
                         XC_FATX_MAX_PATH);
            break;
        case XC_FATX_PROBLEM_TOO_MANY_ENTRIES:
            (void)printf("  %s/: %zu entries in one folder (limit %d)\n",
                         p->path[0] != '\0' ? p->path : ".", p->value, XC_FATX_MAX_DIR_ENTRIES);
            break;
        }
    }
    if (count > MAX_REPORTED) {
        (void)printf("  ... and %zu more\n", count - MAX_REPORTED);
    }
    size_t files = l.file_count;
    uint64_t bytes = l.total_file_bytes;
    xc_xdvdfs_listing_free(&l);
    xc_host_file_close(&hf);
    if (count > 0) {
        (void)printf("not installable: %s (%zu problem%s)\n", xc_err_str(XC_ERR_NOT_INSTALLABLE),
                     count, count == 1 ? "" : "s");
        return 3;
    }
    (void)printf("installable: %zu files, %" PRIu64 " bytes\n", files, bytes);
    return 0;
}

/* Bytes as MiB with one decimal, for reading. */
static double mib(uint64_t bytes)
{
    return (double)bytes / (1024.0 * 1024.0);
}

static int cmd_layout(const char *image)
{
    xc_source src;
    xc_host_file hf = {NULL};
    xc_xdvdfs_volume vol;
    xc_xdvdfs_listing l;
    int rc = load(image, &src, &hf, &vol, &l);
    if (rc != 0) {
        return rc;
    }
    xc_xdvdfs_order o;
    xc_err err = xc_xdvdfs_stream_order(&vol, &l, &o);
    if (err != XC_OK) {
        xc_xdvdfs_listing_free(&l);
        xc_host_file_close(&hf);
        return fail(image, err);
    }
    (void)printf("layout %s, partition at %" PRIu64 " (%.1f MiB), image %" PRIu64 " bytes\n",
                 layout_name(vol.layout), vol.partition_offset, mib(vol.partition_offset),
                 src.size);
    if (o.tables > 0) {
        (void)printf("directory tables: %zu (incl. root), from %" PRIu64 " to %" PRIu64
                     " (%.1f to %.1f MiB)\n",
                     o.tables, o.tables_start, o.tables_end, mib(o.tables_start),
                     mib(o.tables_end));
    }
    if (o.data_start != UINT64_MAX) {
        (void)printf("file data: %zu files, %.1f MiB, from %" PRIu64 " to %" PRIu64
                     " (%.1f to %.1f MiB)\n",
                     l.file_count, mib(l.total_file_bytes), o.data_start, o.data_end,
                     mib(o.data_start), mib(o.data_end));
    }
    if (o.late_files == 0) {
        (void)printf("order: every directory table comes before the files it describes;\n"
                     "       one sequential pass can install it (e.g. from a Deflate zip)\n");
    } else {
        (void)printf("order: %zu files (%.1f MiB) start before a directory table on their path "
                     "has been read\n",
                     o.late_files, mib(o.late_bytes));
        (void)printf("       earliest at %" PRIu64 " (%.1f MiB); the last table they wait for "
                     "ends at %" PRIu64 " (%.1f MiB)\n",
                     o.first_late, mib(o.first_late), o.last_table_end_needed,
                     mib(o.last_table_end_needed));
        (void)printf("       one sequential pass is not enough: a stream would have to be re-read "
                     "from %.1f MiB\n",
                     mib(o.first_late));
    }
    xc_xdvdfs_listing_free(&l);
    xc_host_file_close(&hf);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "layout") == 0) {
        return cmd_layout(argv[2]);
    }
    if ((argc == 3 || argc == 4) && strcmp(argv[1], "check") == 0) {
        return cmd_check(argv[2], argc == 4 ? argv[3] : NULL);
    }
    if (argc == 3 && strcmp(argv[1], "list") == 0) {
        return cmd_list(argv[2]);
    }
    if (argc == 4 && strcmp(argv[1], "extract") == 0) {
        return cmd_extract(argv[2], argv[3]);
    }
    return usage();
}
