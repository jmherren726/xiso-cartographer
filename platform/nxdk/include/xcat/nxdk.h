#ifndef XCAT_NXDK_H
#define XCAT_NXDK_H

/*
 * Xbox (nxdk) platform adapters: a file-backed xc_source, a FATX directory
 * xc_sink and drive helpers (docs/ARCHITECTURE.md §2, §4). Built only into the
 * XBE (app/Makefile). Paths are DOS-style ("E:\Games\Title"); FATX limits paths
 * to 250 bytes after the drive (docs/formats/fatx-names.md §3), so 256-byte
 * buffers always suffice once xc_fatx_check_listing() has passed.
 */

#include <stdbool.h>
#include <stdint.h>

#include "xcat/sink.h"
#include "xcat/source.h"

#define XC_NX_PATH_MAX 256 /* bytes, including the terminator */

typedef struct xc_nx_file {
    void *handle; /* HANDLE */
} xc_nx_file;

/* Opens `path` read-only as a source (64-bit offsets). `f` must outlive `src`. */
xc_err xc_nx_file_open(xc_source *src, xc_nx_file *f, const char *path);
void xc_nx_file_close(xc_nx_file *f);

typedef struct xc_nx_dir_sink {
    char root[XC_NX_PATH_MAX];
    char open_path[XC_NX_PATH_MAX];
    void *handle; /* HANDLE of the open file, or NULL */
    uint32_t expected;
    uint64_t written;
} xc_nx_dir_sink;

/* Writes into `root`, creating it and missing parents. Partial files are deleted. */
xc_err xc_nx_dir_sink_open(xc_sink *sink, xc_nx_dir_sink *ds, const char *root);
void xc_nx_dir_sink_close(xc_nx_dir_sink *ds);

/* Mounts E: (HDD partition 1) if needed. */
bool xc_nx_mount_e(void);

/* Free bytes on the drive containing `path` (e.g. "E:\"). */
xc_err xc_nx_free_space(const char *path, uint64_t *free_bytes);

/* True if `path` names an existing file or directory. */
bool xc_nx_exists(const char *path);

/* Creates `path` and any missing parents. */
bool xc_nx_mkdirs(const char *path);

#endif /* XCAT_NXDK_H */
