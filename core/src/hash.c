/*
 * Whole-image MD5 and SHA-1 over BearSSL (ADR 0004 §5, ADR 0005). BearSSL's state export
 * (br_md5_state / br_sha1_state) gives the chaining value after the last complete 64-byte
 * block, so a saved state stands for the count rounded down to 64 (docs/formats/journal.md §5).
 */
#include "xcat/hash.h"

#include <bearssl_hash.h>
#include <stdlib.h>
#include <string.h>

struct xc_image_hash {
    br_md5_context md5;
    br_sha1_context sha1;
};

xc_err xc_image_hash_create(xc_image_hash **out)
{
    if (out == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    xc_image_hash *h = malloc(sizeof *h);
    if (h == NULL) {
        return XC_ERR_NOMEM;
    }
    br_md5_init(&h->md5);
    br_sha1_init(&h->sha1);
    *out = h;
    return XC_OK;
}

void xc_image_hash_free(xc_image_hash *h)
{
    free(h);
}

void xc_image_hash_update(xc_image_hash *h, const void *data, size_t len)
{
    br_md5_update(&h->md5, data, len);
    br_sha1_update(&h->sha1, data, len);
}

uint64_t xc_image_hash_count(const xc_image_hash *h)
{
    uint8_t scratch[XC_MD5_BYTES];
    return br_md5_state(&h->md5, scratch);
}

uint64_t xc_image_hash_save(const xc_image_hash *h, uint8_t md5_state[XC_MD5_BYTES],
                            uint8_t sha1_state[XC_SHA1_BYTES])
{
    uint64_t count = br_md5_state(&h->md5, md5_state);
    (void)br_sha1_state(&h->sha1, sha1_state);
    return count & ~(uint64_t)(XC_HASH_BLOCK - 1U);
}

xc_err xc_image_hash_restore(xc_image_hash *h, uint64_t offset,
                             const uint8_t md5_state[XC_MD5_BYTES],
                             const uint8_t sha1_state[XC_SHA1_BYTES])
{
    if (offset % XC_HASH_BLOCK != 0) {
        return XC_ERR_INVALID_ARG;
    }
    br_md5_init(&h->md5);
    br_sha1_init(&h->sha1);
    br_md5_set_state(&h->md5, md5_state, offset);
    br_sha1_set_state(&h->sha1, sha1_state, offset);
    return XC_OK;
}

void xc_image_hash_final(const xc_image_hash *h, uint8_t md5[XC_MD5_BYTES],
                         uint8_t sha1[XC_SHA1_BYTES])
{
    br_md5_out(&h->md5, md5);
    br_sha1_out(&h->sha1, sha1);
}

void xc_hex_lower(const uint8_t *bytes, size_t len, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[2 * i] = digits[bytes[i] >> 4U];
        out[2 * i + 1] = digits[bytes[i] & 0xFU];
    }
    out[2 * len] = '\0';
}

static int nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool xc_hex_parse(const char *hex, uint8_t *bytes, size_t len)
{
    if (hex == NULL || strlen(hex) != 2 * len) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        int hi = nibble(hex[2 * i]);
        int lo = nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        bytes[i] = (uint8_t)((unsigned)hi << 4U | (unsigned)lo);
    }
    return true;
}
