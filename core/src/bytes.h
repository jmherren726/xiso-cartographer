#ifndef XCAT_BYTES_H
#define XCAT_BYTES_H

/*
 * Bounds-checked little-endian reads (HANDOFF §7.3). Each returns false, and
 * leaves *out unchanged, unless [off, off + N) lies inside buf[0, len).
 * Internal to libxcat; never cast raw pointers to structures instead.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static inline bool xc_rd_u8(const uint8_t *buf, size_t len, size_t off, uint8_t *out)
{
    if (off >= len) {
        return false;
    }
    *out = buf[off];
    return true;
}

static inline bool xc_rd_u16le(const uint8_t *buf, size_t len, size_t off, uint16_t *out)
{
    if (len < 2 || off > len - 2) {
        return false;
    }
    *out = (uint16_t)((unsigned)buf[off] | ((unsigned)buf[off + 1] << 8U));
    return true;
}

static inline bool xc_rd_u32le(const uint8_t *buf, size_t len, size_t off, uint32_t *out)
{
    if (len < 4 || off > len - 4) {
        return false;
    }
    *out = (uint32_t)buf[off] | ((uint32_t)buf[off + 1] << 8U) | ((uint32_t)buf[off + 2] << 16U) |
           ((uint32_t)buf[off + 3] << 24U);
    return true;
}

static inline bool xc_rd_u64le(const uint8_t *buf, size_t len, size_t off, uint64_t *out)
{
    uint32_t lo = 0;
    uint32_t hi = 0;
    if (!xc_rd_u32le(buf, len, off, &lo) || !xc_rd_u32le(buf, len, off + 4, &hi)) {
        return false;
    }
    *out = (uint64_t)lo | ((uint64_t)hi << 32U);
    return true;
}

/* True if [off, off + n) lies inside buf[0, len). */
static inline bool xc_in_bounds(size_t len, size_t off, size_t n)
{
    return off <= len && n <= len - off;
}

#endif /* XCAT_BYTES_H */
