#ifndef XCAT_ARCHIVEORG_H
#define XCAT_ARCHIVEORG_H

/*
 * archive.org item listings (docs/formats/archiveorg.md): reads the answer of
 * https://archive.org/metadata/<identifier> and chooses the item's image files, each with
 * the name, URL, size and hashes a map entry needs (ADR 0013). Core never fetches anything;
 * the caller passes the response body.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xcat/error.h"
#include "xcat/hash.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XC_ARCHIVEORG_MAX_METADATA ((size_t)4 * 1024 * 1024) /* §1, response body */
#define XC_ARCHIVEORG_MAX_ID 100                             /* §1 */
#define XC_ARCHIVEORG_MAX_NAME 512                           /* §3.1, bytes */
#define XC_ARCHIVEORG_MAX_FILES 10000                        /* §5, the map's entry limit */

typedef struct xc_archiveorg_file {
    char *path; /* the file's name in the item, folders included */
    char *name; /* §4: the last segment without its image or zip extension */
    char *url;  /* §4: https://archive.org/download/<identifier>/<path, percent-encoded> */
    uint64_t size;
    bool has_md5;
    bool has_sha1;
    bool has_crc32;
    uint8_t md5[XC_MD5_BYTES];
    uint8_t sha1[XC_SHA1_BYTES];
    uint32_t crc32;
    bool zip;          /* the name ends in .zip */
    bool private_file; /* "private": "true" (§3.3; informational) */
} xc_archiveorg_file;

typedef enum xc_archiveorg_status {
    XC_ARCHIVEORG_OK,           /* files[] holds the chosen files (maybe none) */
    XC_ARCHIVEORG_NOT_METADATA, /* §2: not JSON, not an object, or files isn't an array */
    XC_ARCHIVEORG_DARK,         /* §2: "is_dark": true */
    XC_ARCHIVEORG_MISSING,      /* §2: no files member, e.g. {} */
} xc_archiveorg_status;

typedef struct xc_archiveorg_item {
    xc_archiveorg_status status;
    xc_archiveorg_file *files;
    size_t file_count;
    /* Image files skipped for a bad name or size (§3), and the first one's path (control
     * characters as '?', cut at XC_ARCHIVEORG_MAX_NAME bytes), or "" if none. */
    size_t invalid;
    char first_invalid[XC_ARCHIVEORG_MAX_NAME + 1];
    size_t hashes_dropped; /* malformed hashes ignored (§3.3) */
    bool truncated;        /* more than XC_ARCHIVEORG_MAX_FILES were chosen; the rest dropped */
} xc_archiveorg_item;

/* True for a valid identifier (§1). */
bool xc_archiveorg_valid_id(const char *id);

/* Writes "https://archive.org/metadata/<id>". XC_ERR_INVALID_ARG for a bad identifier,
 * XC_ERR_LIMIT if cap is too small. */
xc_err xc_archiveorg_metadata_url(const char *id, char *out, size_t cap);

/*
 * Reads a metadata response for item `id`. XC_OK whenever the body was understood well
 * enough to say what it is: item->status tells which (§2); XC_ERR_INVALID_ARG for a bad
 * identifier, XC_ERR_LIMIT for a body over XC_ARCHIVEORG_MAX_METADATA, XC_ERR_NOMEM. Free
 * the item with xc_archiveorg_free in every case.
 */
xc_err xc_archiveorg_parse(const char *id, const char *body, size_t len, xc_archiveorg_item *item);
void xc_archiveorg_free(xc_archiveorg_item *item);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_ARCHIVEORG_H */
