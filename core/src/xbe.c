/* XBE certificates (xcat/xbe.h, docs/formats/xbe.md). */
#include "xcat/xbe.h"

#include <string.h>

#include "bytes.h"

#define HEADER_MIN 0x178U /* §3: the image header up to the fields we read */
#define CERT_MIN 0x5CU    /* §3: the certificate up to the end of the title name */

/* §3: the title name as UTF-8, unrepresentable units and control characters as '?'. */
static void title_utf8(const uint8_t *units, char *out)
{
    size_t n = 0;
    for (size_t i = 0; i < XC_XBE_TITLE_UNITS; i++) {
        uint32_t u = (uint32_t)units[2 * i] | (uint32_t)units[2 * i + 1] << 8U;
        if (u == 0) {
            break;
        }
        if ((u >= 0xD800U && u <= 0xDFFFU) || u < 0x20U || (u >= 0x7FU && u <= 0x9FU)) {
            out[n++] = '?';
        } else if (u < 0x80U) {
            out[n++] = (char)u;
        } else if (u < 0x800U) {
            out[n++] = (char)(0xC0U | (u >> 6U));
            out[n++] = (char)(0x80U | (u & 0x3FU));
        } else {
            out[n++] = (char)(0xE0U | (u >> 12U));
            out[n++] = (char)(0x80U | ((u >> 6U) & 0x3FU));
            out[n++] = (char)(0x80U | (u & 0x3FU));
        }
    }
    while (n > 0 && out[n - 1] == ' ') {
        n--;
    }
    out[n] = '\0';
}

xc_err xc_xbe_read_cert(const xc_source *src, xc_xbe_cert *cert)
{
    memset(cert, 0, sizeof *cert);
    if (src->size < HEADER_MIN) {
        return XC_ERR_CORRUPT;
    }
    uint8_t head[HEADER_MIN];
    xc_err err = xc_source_read(src, 0, head, sizeof head);
    if (err != XC_OK) {
        return err;
    }
    uint32_t base = 0;
    uint32_t headers = 0;
    uint32_t cert_addr = 0;
    (void)xc_rd_u32le(head, sizeof head, 0x104, &base);
    (void)xc_rd_u32le(head, sizeof head, 0x108, &headers);
    (void)xc_rd_u32le(head, sizeof head, 0x118, &cert_addr);
    if (memcmp(head, "XBEH", 4) != 0 || headers < HEADER_MIN || headers > src->size ||
        cert_addr < base) {
        return XC_ERR_CORRUPT;
    }
    uint32_t off = cert_addr - base;
    if (off < HEADER_MIN || off > headers || headers - off < CERT_MIN) {
        return XC_ERR_CORRUPT;
    }
    uint8_t c[CERT_MIN];
    err = xc_source_read(src, off, c, sizeof c);
    if (err != XC_OK) {
        return err;
    }
    uint32_t size = 0;
    (void)xc_rd_u32le(c, sizeof c, 0, &size);
    if (size < CERT_MIN) {
        return XC_ERR_CORRUPT;
    }
    (void)xc_rd_u32le(c, sizeof c, 8, &cert->title_id);
    title_utf8(c + 0x0C, cert->title);
    return XC_OK;
}
