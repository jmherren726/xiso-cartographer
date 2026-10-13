#ifndef XCAT_HOST_H
#define XCAT_HOST_H

/*
 * Host (PC) platform adapters for tests and xcat-cli: a file-backed xc_source
 * and a directory xc_sink. Never built for the Xbox (docs/ARCHITECTURE.md §2).
 *
 * Paths are UTF-8. On Windows they are converted to UTF-16 and given the
 * "\\?\" long-path prefix, so paths over MAX_PATH (e.g. 255-character XDVDFS
 * names) work without the LongPathsEnabled policy.
 */

#include <stdio.h>

#include "xcat/sink.h"
#include "xcat/source.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct xc_host_file {
    FILE *f;
} xc_host_file;

/* Opens `path` read-only as a source (64-bit offsets). `hf` must outlive `src`. */
xc_err xc_host_file_open(xc_source *src, xc_host_file *hf, const char *path);
void xc_host_file_close(xc_host_file *hf);

typedef struct xc_host_dir_sink {
    char *root;      /* owned */
    char *open_path; /* full path of the open file; owned */
    FILE *f;         /* the open file, or NULL */
    uint32_t expected;
    uint64_t written;
} xc_host_dir_sink;

/*
 * Writes into directory `root`, creating it and any missing parents. Existing
 * files are overwritten; partial files are removed.
 */
xc_err xc_host_dir_sink_open(xc_sink *sink, xc_host_dir_sink *ds, const char *root);
void xc_host_dir_sink_close(xc_host_dir_sink *ds);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_HOST_H */
