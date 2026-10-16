#ifndef XCAT_CACERT_H
#define XCAT_CACERT_H

/*
 * The built-in CA bundle (ADR 0012 §3): third_party/cacert/cacert.pem, embedded by
 * tools/cacert/embed_cacert.py into a generated cacert_data.c at build time.
 */

#include <stddef.h>

extern const unsigned char xc_cacert_pem[];
extern const size_t xc_cacert_pem_len;
extern const char xc_cacert_date[]; /* "Certificate data from Mozilla as of: ..." */

#endif /* XCAT_CACERT_H */
