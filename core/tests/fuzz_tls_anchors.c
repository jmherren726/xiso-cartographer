/*
 * libFuzzer target for the trust-anchor loader (xcat/tls.h, ADR 0012 §4): the PEM bundle
 * that E:\Apps\xcat\cacert.pem may replace the built-in one with.
 *
 * Every input is parsed as a bundle. It must either fail with one of the documented errors,
 * leaving nothing behind, or succeed with 1..XC_TLS_MAX_ANCHORS anchors; a violation aborts.
 * The sanitizers catch any read or write outside the input and the anchor arena.
 *
 * Seeds are in core/tests/fuzz-seeds/tls/: the test certificates (tools/fixtures/tls/).
 */
#include <stdint.h>
#include <stdlib.h>

#include "xcat/tls.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void check(int ok)
{
    if (!ok) {
        abort();
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    xc_tls_anchors *a = NULL;
    size_t bad = 0;
    xc_err err = xc_tls_anchors_parse((const char *)data, size, &a, &bad);
    if (err == XC_OK) {
        size_t n = xc_tls_anchors_count(a);
        check(a != NULL && n >= 1 && n <= XC_TLS_MAX_ANCHORS && bad == 0);
    } else {
        check(a == NULL);
        check(err == XC_ERR_CORRUPT || err == XC_ERR_LIMIT || err == XC_ERR_NOMEM);
    }
    xc_tls_anchors_free(a);
    return 0;
}
