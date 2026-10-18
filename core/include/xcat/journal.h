#ifndef XCAT_JOURNAL_H
#define XCAT_JOURNAL_H

/*
 * Install journal (docs/formats/journal.md): which image an unfinished install
 * comes from, and which of its files are already on disk. This module formats
 * the lines and reads a journal back against the image's listing; the installer
 * does the file I/O and the flushing (§2).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xcat/error.h"
#include "xcat/hash.h"
#include "xcat/xdvdfs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XC_JOURNAL_NAME ".xcat-journal"
#define XC_JOURNAL_MAX_FILE ((size_t)4 * 1024 * 1024) /* §3.1 */
#define XC_JOURNAL_MAX_LINE 8192                      /* §3.1, without the LF */
#define XC_JOURNAL_MAX_VALIDATOR 127                  /* as xc_http_resource's (http.md §4.3) */
#define XC_JOURNAL_VERSION_MAJOR 1
#define XC_JOURNAL_VERSION_MINOR 2 /* 1.1: part checkpoints (ADR 0011); 1.2: hash state */

/* §4: what an install is from. */
typedef struct xc_journal_id {
    uint64_t image_size;
    uint64_t partition_offset;
    uint32_t listing_crc;
    uint64_t file_count;
    char etag[XC_JOURNAL_MAX_VALIDATOR + 1];          /* "" if the server sent none */
    char last_modified[XC_JOURNAL_MAX_VALIDATOR + 1]; /* likewise */
} xc_journal_id;

/* §4: the listing digest. */
uint32_t xc_journal_listing_crc(const xc_xdvdfs_listing *listing);

/* Fills in `id` for an image; NULL validators mean "none". Longer ones are ignored (http.md). */
void xc_journal_make_id(xc_journal_id *id, const xc_xdvdfs_listing *listing,
                        uint64_t partition_offset, uint64_t image_size, const char *etag,
                        const char *last_modified);

/* §4: same image? Validators only count where both sides have one. */
bool xc_journal_same_image(const xc_journal_id *a, const xc_journal_id *b);

/*
 * The header and identity lines (§2.2, §4), written once when an install starts.
 * XC_ERR_LIMIT if they don't fit in out[0, cap).
 */
xc_err xc_journal_format_start(const xc_journal_id *id, const char *source_url, char *out,
                               size_t cap, size_t *len);

/* One `done` line (§5). XC_ERR_LIMIT if it doesn't fit. */
xc_err xc_journal_format_done(const char *path, uint32_t size, uint32_t crc, char *out, size_t cap,
                              size_t *len);

/* One `part` checkpoint line (§5, v1.1): the first `bytes` bytes, with CRC-32 `crc`, are on disk.
 */
xc_err xc_journal_format_part(const char *path, uint32_t bytes, uint32_t crc, char *out, size_t cap,
                              size_t *len);

/* One `hash` line (§5, v1.2): every image byte before `offset` (a multiple of 64) is hashed. */
xc_err xc_journal_format_hash(uint64_t offset, const uint8_t md5_state[XC_MD5_BYTES],
                              const uint8_t sha1_state[XC_SHA1_BYTES], char *out, size_t cap,
                              size_t *len);

/* What a journal says about the image it's read against (§6). */
typedef struct xc_journal_state {
    bool valid;           /* false: invalid or another image; don't reuse the folder (§6.4) */
    const char *why;      /* when !valid: a short reason for messages */
    xc_journal_id id;     /* as read (when the identity lines were readable) */
    bool *done;           /* [listing file index]: a valid done line exists */
    uint32_t *crc;        /* [listing file index]: its CRC-32, when done */
    uint32_t *part_bytes; /* [listing file index]: last checkpoint of an unfinished file, or 0 */
    uint32_t *part_crc;   /* [listing file index]: CRC-32 of those bytes */
    size_t done_count;
    bool has_hash;        /* a valid `hash` line was read (the last one counts) */
    uint64_t hash_offset; /* its offset */
    uint8_t hash_md5[XC_MD5_BYTES];
    uint8_t hash_sha1[XC_SHA1_BYTES];
    bool torn; /* reading stopped at a torn line (a power cut during an append) */
} xc_journal_state;

/*
 * Reads data[0, len) against the listing and identity of the image about to be
 * installed. Returns XC_OK with state->valid saying whether the install can be
 * resumed in this folder, or XC_ERR_NOMEM. Free with xc_journal_state_free.
 */
xc_err xc_journal_read(const char *data, size_t len, const xc_xdvdfs_listing *listing,
                       const xc_journal_id *image, xc_journal_state *state);
void xc_journal_state_free(xc_journal_state *state);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_JOURNAL_H */
