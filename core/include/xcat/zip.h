#ifndef XCAT_ZIP_H
#define XCAT_ZIP_H

/*
 * Zip archives read through an xc_source (docs/formats/zip.md, ADR 0009): find the central
 * directory (Zip64 too), list the entries, choose the image and find where its data starts.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xcat/error.h"
#include "xcat/source.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XC_ZIP_MAX_CD_SIZE (4U * 1024U * 1024U) /* zip.md §4.2 */
#define XC_ZIP_MAX_ENTRIES 16384U               /* zip.md §4.2 */

#define XC_ZIP_STORED 0U
#define XC_ZIP_DEFLATE 8U

/* One central directory entry (zip.md §4). */
typedef struct xc_zip_entry {
    char *name; /* NUL-terminated; holds no NUL itself */
    uint16_t method;
    uint16_t flags;
    uint32_t crc32;
    uint64_t compressed_size;
    uint64_t size; /* uncompressed */
    uint64_t local_offset;
} xc_zip_entry;

typedef struct xc_zip_listing {
    xc_zip_entry *entries;
    size_t count;
    uint64_t cd_offset; /* every member's data ends at or before this */
    bool zip64;         /* true if the archive has Zip64 end records (zip.md §3) */
} xc_zip_listing;

/* The chosen member, ready to read (zip.md §6). */
typedef struct xc_zip_member {
    size_t index; /* into the listing */
    uint16_t method;
    uint32_t crc32;
    uint64_t compressed_size;
    uint64_t size;
    uint64_t data_offset; /* the first byte of the member's (compressed) data */
} xc_zip_member;

/*
 * Reads the end records and the central directory (zip.md §2–§4). XC_ERR_NOT_ZIP,
 * XC_ERR_CORRUPT, XC_ERR_LIMIT, XC_ERR_ZIP_UNSUPPORTED (split archives), XC_ERR_NOMEM or a
 * read error. On success the caller frees `out` with xc_zip_listing_free(); on failure it
 * is left empty.
 */
xc_err xc_zip_list(const xc_source *src, xc_zip_listing *out);

/* Frees everything in a listing and zeroes it. Safe on a zeroed listing. */
void xc_zip_listing_free(xc_zip_listing *listing);

/* True if the entry's name ends in .iso or .xiso and it isn't a directory (zip.md §5.1). */
bool xc_zip_is_image_name(const char *name);

/* The one image entry (zip.md §5); XC_ERR_ZIP_MEMBER if there are none or several. */
xc_err xc_zip_pick_image(const xc_zip_listing *listing, size_t *index);

/*
 * Checks entry `index` and its local header (zip.md §6) and fills `out`.
 * XC_ERR_ZIP_UNSUPPORTED (method or encryption), XC_ERR_CORRUPT, or a read error.
 */
xc_err xc_zip_open_member(const xc_source *src, const xc_zip_listing *listing, size_t index,
                          xc_zip_member *out);

/* "stored", "Deflate", "Deflate64", "LZMA", "AES-encrypted" ... or NULL if not known. */
const char *xc_zip_method_name(uint16_t method);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_ZIP_H */
