#ifndef XCAT_CRC32_H
#define XCAT_CRC32_H

/*
 * CRC-32 as used by zip, gzip and PNG (ISO-HDLC: reflected polynomial
 * 0xEDB88320, initial value and final XOR 0xFFFFFFFF). Same result as Python's
 * zlib.crc32(). Used to check downloads (M3) and zip members (ADR 0009).
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Continues a CRC over `len` more bytes: start with crc = 0, then pass each
 * result back in. xc_crc32(xc_crc32(0, a, n), b, m) == CRC of a followed by b.
 */
uint32_t xc_crc32(uint32_t crc, const void *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_CRC32_H */
