/*
 * xcat-cli: run libxcat on a PC.
 *
 *   xcat-cli list <image>               print the layout and every file (offset order)
 *   xcat-cli extract <image> <out-dir>  extract the image into out-dir
 *
 * Exit status: 0 success, 1 failure, 2 usage error.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xcat/extract.h"
#include "xcat/host.h"
#include "xcat/version.h"
#include "xcat/xdvdfs.h"

#define IO_BUFFER_SIZE ((size_t)1024 * 1024)

static int usage(void)
{
    (void)fprintf(stderr,
                  "xcat-cli %s\n"
                  "usage: xcat-cli list <image>\n"
                  "       xcat-cli extract <image> <out-dir>\n",
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

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "list") == 0) {
        return cmd_list(argv[2]);
    }
    if (argc == 4 && strcmp(argv[1], "extract") == 0) {
        return cmd_extract(argv[2], argv[3]);
    }
    return usage();
}
