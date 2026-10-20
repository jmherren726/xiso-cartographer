#ifndef XCAT_XBE_H
#define XCAT_XBE_H

/*
 * The certificate of an Xbox executable (docs/formats/xbe.md): the title ID and name of an
 * installed game's default.xbe. Only header fields are read; the file is untrusted.
 */

#include <stdint.h>

#include "xcat/error.h"
#include "xcat/source.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XC_XBE_TITLE_UNITS 40 /* §2: UTF-16 code units in the title name */

typedef struct xc_xbe_cert {
    uint32_t title_id;
    char title[3 * XC_XBE_TITLE_UNITS + 1]; /* §3: UTF-8, may be "" */
} xc_xbe_cert;

/* Reads the certificate. XC_ERR_CORRUPT if the file isn't an XBE by §3; errors from the
 * source as they come. */
xc_err xc_xbe_read_cert(const xc_source *src, xc_xbe_cert *cert);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_XBE_H */
