#ifndef XCAT_HASH_H
#define XCAT_HASH_H

/*
 * Whole-image MD5 and SHA-1 (ADR 0004 §5, docs/formats/journal.md §5 and §7), computed while
 * an install reads the image front to back. The running state can be saved at any time, as it
 * stood at the last 64-byte boundary, and restored after a power cut (the journal's `hash`
 * records). Backed by BearSSL (ADR 0005).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xcat/error.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XC_MD5_BYTES 16U
#define XC_SHA1_BYTES 20U
#define XC_HASH_BLOCK 64U /* a saved state's offset is a multiple of this */

typedef struct xc_image_hash xc_image_hash;

/* Both hashes over zero bytes. XC_ERR_NOMEM if allocation fails. */
xc_err xc_image_hash_create(xc_image_hash **out);
void xc_image_hash_free(xc_image_hash *h);

void xc_image_hash_update(xc_image_hash *h, const void *data, size_t len);

/* Bytes hashed so far. */
uint64_t xc_image_hash_count(const xc_image_hash *h);

/*
 * The running state at the last 64-byte boundary: returns that offset (the count rounded
 * down to a multiple of 64) and writes both chaining values.
 */
uint64_t xc_image_hash_save(const xc_image_hash *h, uint8_t md5_state[XC_MD5_BYTES],
                            uint8_t sha1_state[XC_SHA1_BYTES]);

/* Restores a saved state; the next byte hashed is the one at `offset`. XC_ERR_INVALID_ARG
 * if `offset` isn't a multiple of 64. */
xc_err xc_image_hash_restore(xc_image_hash *h, uint64_t offset,
                             const uint8_t md5_state[XC_MD5_BYTES],
                             const uint8_t sha1_state[XC_SHA1_BYTES]);

/* The digests of everything hashed so far (the running state is unchanged). */
void xc_image_hash_final(const xc_image_hash *h, uint8_t md5[XC_MD5_BYTES],
                         uint8_t sha1[XC_SHA1_BYTES]);

/* Lower-case hex (as archive.org's metadata writes them): out needs 2 * len + 1 bytes. */
void xc_hex_lower(const uint8_t *bytes, size_t len, char *out);

/* Parses `len` bytes of hex, either case, exactly 2 * len digits. */
bool xc_hex_parse(const char *hex, uint8_t *bytes, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_HASH_H */
