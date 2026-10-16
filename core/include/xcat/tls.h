#ifndef XCAT_TLS_H
#define XCAT_TLS_H

/*
 * TLS client on BearSSL (ADR 0005, ADR 0012 §2): an xc_net that runs TLS 1.2 over another
 * xc_net (plain TCP). Pass it as xc_http_config.tls and https URLs work. Certificates are
 * always checked: against the trust anchors, the URL's host name and the current time.
 * There is no way to turn that off.
 *
 * BearSSL itself stays inside this module (and hash.c); nothing here exposes its types.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xcat/error.h"
#include "xcat/net.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Platform hooks (docs/ARCHITECTURE.md §4) ---------------------------------------- */

/*
 * Seed material for the TLS random generator (ADR 0012 §6). Fills buf[0, len) with bytes
 * that an attacker can't predict, or fails (XC_ERR_TLS_NO_RANDOM) if it can't yet: TLS then
 * refuses to connect. Asked for XC_TLS_SEED_BYTES before each connection.
 */
typedef struct xc_entropy {
    xc_err (*get)(void *ctx, void *buf, size_t len);
    void *ctx;
} xc_entropy;

/*
 * The current time for certificate checks, as seconds since 1970-01-01 UTC. Fails with
 * XC_ERR_TLS_NO_TIME if no acceptable time is known (ADR 0012 §5). Asked before each
 * connection.
 */
typedef struct xc_wallclock {
    xc_err (*now)(void *ctx, int64_t *unix_s);
    void *ctx;
} xc_wallclock;

#define XC_TLS_SEED_BYTES 32U

/* ---- Trust anchors ------------------------------------------------------------------- */

#define XC_TLS_MAX_ANCHOR_FILE ((size_t)1024 * 1024) /* ADR 0012 §4 */
#define XC_TLS_MAX_ANCHORS 512U                      /* ADR 0012 §4 */
#define XC_TLS_MAX_CERT ((size_t)16 * 1024)          /* one certificate's DER encoding */

typedef struct xc_tls_anchors xc_tls_anchors;

/*
 * Decodes a PEM bundle of certificates (e.g. curl's cacert.pem) into trust anchors, allocated
 * once here. Text outside the BEGIN/END lines is ignored. Each object must be a
 * "CERTIFICATE" (or "X509 CERTIFICATE") holding an RSA or EC key; a CA certificate anchors
 * chains it issued, any other certificate is trusted as that exact server certificate.
 *
 * All or nothing: XC_ERR_CORRUPT for anything malformed or unsupported, XC_ERR_LIMIT past
 * XC_TLS_MAX_ANCHOR_FILE, XC_TLS_MAX_ANCHORS or XC_TLS_MAX_CERT, XC_ERR_NOMEM. A bundle with
 * no certificate is XC_ERR_CORRUPT. On failure, *bad_cert (if not NULL) is the 1-based number
 * of the object that was rejected, or 0 if the problem isn't one object's.
 */
xc_err xc_tls_anchors_parse(const char *pem, size_t len, xc_tls_anchors **out, size_t *bad_cert);
/*
 * The built-in bundle (ADR 0012 §3): Mozilla's root store as curl publishes it, pinned in
 * third_party/cacert/ and decoded here with xc_tls_anchors_parse. A file the user puts in its
 * place (E:\Apps\xcat\cacert.pem on the console, --ca-file in xcat-cli) replaces it (§4).
 */
xc_err xc_tls_anchors_builtin(xc_tls_anchors **out);
/* When Mozilla's data in the built-in bundle is from, e.g. "Fri Sep 25 03:12:01 2026 GMT". */
const char *xc_tls_builtin_date(void);

size_t xc_tls_anchors_count(const xc_tls_anchors *a);
void xc_tls_anchors_free(xc_tls_anchors *a);

/* ---- The TLS net --------------------------------------------------------------------- */

typedef struct xc_tls_config {
    const xc_net *tcp;             /* the plain connections TLS runs over; required */
    const xc_tls_anchors *anchors; /* required; must outlive the xc_tls */
    const xc_entropy *entropy;     /* required */
    const xc_wallclock *wallclock; /* required */
} xc_tls_config;

typedef struct xc_tls xc_tls;

/*
 * Allocates a TLS client for one connection at a time: about 40 KB, all of it now (HANDOFF
 * §7.2). Fills in *net, which stays valid until xc_tls_destroy.
 */
xc_err xc_tls_create(const xc_tls_config *cfg, xc_tls **out, xc_net *net);
void xc_tls_destroy(xc_tls *t);

/* What the last handshake agreed on (for logs and the M5 benchmark). */
typedef struct xc_tls_info {
    unsigned version;       /* 0x0303 for TLS 1.2; 0 if no handshake has completed */
    uint16_t cipher_suite;  /* IANA number */
    const char *suite_name; /* e.g. "ECDHE-ECDSA-CHACHA20-POLY1305"; NULL before a handshake */
} xc_tls_info;
void xc_tls_last_info(const xc_tls *t, xc_tls_info *info);

/* BearSSL's error code for the last failure (0 if none), for logs; see bearssl_ssl.h. */
int xc_tls_last_error(const xc_tls *t);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_TLS_H */
