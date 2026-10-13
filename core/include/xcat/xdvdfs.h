#ifndef XCAT_XDVDFS_H
#define XCAT_XDVDFS_H

/*
 * Xbox disc filesystem (XDVDFS) reader. Behaviour is specified in
 * docs/formats/xdvdfs.md; every input is treated as hostile.
 */

#include <stddef.h>
#include <stdint.h>

#include "xcat/error.h"
#include "xcat/source.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum xc_xdvdfs_layout {
    XC_XDVDFS_XISO, /* trimmed image: partition at 0 */
    XC_XDVDFS_XGD1, /* full disc, partition at 0x18300000 */
    XC_XDVDFS_XGD2, /* full disc, partition at 0x0FD90000 */
    XC_XDVDFS_XGD3  /* full disc, partition at 0x02080000 */
} xc_xdvdfs_layout;

typedef struct xc_xdvdfs_volume {
    xc_xdvdfs_layout layout;
    uint64_t partition_offset; /* absolute byte offset of the game partition */
    uint32_t root_sector;      /* partition-relative */
    uint32_t root_size;        /* bytes; 0 with root_sector 0 = no files */
    uint64_t filetime;         /* creation time, Windows FILETIME */
} xc_xdvdfs_volume;

typedef struct xc_xdvdfs_limits {
    uint32_t max_entries;  /* files + directories */
    uint32_t max_depth;    /* root directory = 0 */
    uint32_t max_path_len; /* bytes, excluding the terminator */
} xc_xdvdfs_limits;

typedef struct xc_xdvdfs_entry {
    const char *path;   /* '/'-separated, relative to the root; owned by the listing */
    uint64_t offset;    /* absolute offset of the data (file) or table (directory) */
    uint32_t size;      /* file size, or the directory's table size */
    uint32_t sector;    /* partition-relative start sector */
    uint8_t attributes; /* raw XDVDFS attributes; 0x10 = directory */
} xc_xdvdfs_entry;

typedef struct xc_xdvdfs_listing {
    xc_xdvdfs_entry *files; /* sorted by offset, ties by path */
    size_t file_count;
    xc_xdvdfs_entry *dirs; /* parents before children; includes empty directories */
    size_t dir_count;
    uint64_t total_file_bytes;
    char *paths_; /* storage for every path; internal */
} xc_xdvdfs_listing;

/* Fills `out` with the defaults from docs/formats/xdvdfs.md §6. */
void xc_xdvdfs_default_limits(xc_xdvdfs_limits *out);

/*
 * Finds the volume descriptor (xiso, then XGD1/2/3) and validates it.
 * XC_ERR_NOT_XDVDFS if no layout matches; XC_ERR_CORRUPT if one matches but is invalid.
 */
xc_err xc_xdvdfs_open(const xc_source *src, xc_xdvdfs_volume *out);

/*
 * Walks every directory and returns all files and directories. `limits` may be
 * NULL for the defaults. On success the caller frees `out` with
 * xc_xdvdfs_listing_free(); on failure `out` is left empty.
 */
xc_err xc_xdvdfs_list(const xc_source *src, const xc_xdvdfs_volume *vol,
                      const xc_xdvdfs_limits *limits, xc_xdvdfs_listing *out);

/* Frees everything in a listing and zeroes it. Safe on a zeroed listing. */
void xc_xdvdfs_listing_free(xc_xdvdfs_listing *listing);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_XDVDFS_H */
