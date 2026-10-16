/*
 * TLS client on BearSSL: ADR 0005 and ADR 0012 §2 (profile), §4 (trust anchors).
 *
 * BearSSL is built without any way to find randomness or the time by itself
 * (core/CMakeLists.txt, app/Makefile), so both come from the platform hooks in
 * xc_tls_config, every time, or the connection is refused.
 */

#include "xcat/tls.h"

#include <stdlib.h>
#include <string.h>

#include "bearssl.h"
#include "cacert.h"

/* Overwrites memory in a way the compiler can't drop as a dead store. */
static void wipe(void *p, size_t len)
{
    volatile unsigned char *v = p;
    while (len-- > 0) {
        *v++ = 0;
    }
}

/* ---- Trust anchors ------------------------------------------------------------------- */

struct xc_tls_anchors {
    size_t count;
    br_x509_trust_anchor *ta; /* XC_TLS_MAX_ANCHORS entries */
    unsigned char *arena;     /* DNs and keys */
    size_t used;
    size_t cap;
    bool overflow;
};

size_t xc_tls_anchors_count(const xc_tls_anchors *a)
{
    return a != NULL ? a->count : 0;
}

void xc_tls_anchors_free(xc_tls_anchors *a)
{
    if (a != NULL) {
        free(a->ta);
        free(a->arena);
        free(a);
    }
}

static unsigned char *arena_put(xc_tls_anchors *a, const void *data, size_t len)
{
    if (a->overflow || len > a->cap - a->used) {
        a->overflow = true;
        return NULL;
    }
    unsigned char *p = a->arena + a->used;
    if (len > 0) {
        memcpy(p, data, len);
    }
    a->used += len;
    return p;
}

static void dn_append(void *ctx, const void *buf, size_t len)
{
    (void)arena_put(ctx, buf, len);
}

typedef struct der_buf {
    unsigned char *data; /* XC_TLS_MAX_CERT bytes */
    size_t len;
    bool overflow;
} der_buf;

static void der_append(void *ctx, const void *buf, size_t len)
{
    der_buf *d = ctx;
    if (d->overflow || len > XC_TLS_MAX_CERT - d->len) {
        d->overflow = true;
        return;
    }
    memcpy(d->data + d->len, buf, len);
    d->len += len;
}

/* Decodes one certificate into the next anchor. */
static xc_err add_anchor(xc_tls_anchors *a, const der_buf *der)
{
    if (a->count >= XC_TLS_MAX_ANCHORS) {
        return XC_ERR_LIMIT;
    }
    br_x509_decoder_context dc;
    size_t dn_start = a->used;
    br_x509_decoder_init(&dc, dn_append, a);
    br_x509_decoder_push(&dc, der->data, der->len);
    br_x509_pkey *pk = br_x509_decoder_get_pkey(&dc);
    if (br_x509_decoder_last_error(&dc) != 0 || pk == NULL) {
        return XC_ERR_CORRUPT;
    }
    br_x509_trust_anchor *ta = &a->ta[a->count];
    memset(ta, 0, sizeof *ta);
    ta->dn.data = a->arena + dn_start;
    ta->dn.len = a->used - dn_start;
    ta->flags = br_x509_decoder_isCA(&dc) ? BR_X509_TA_CA : 0;
    ta->pkey.key_type = pk->key_type;
    if (pk->key_type == BR_KEYTYPE_RSA) {
        ta->pkey.key.rsa.n = arena_put(a, pk->key.rsa.n, pk->key.rsa.nlen);
        ta->pkey.key.rsa.nlen = pk->key.rsa.nlen;
        ta->pkey.key.rsa.e = arena_put(a, pk->key.rsa.e, pk->key.rsa.elen);
        ta->pkey.key.rsa.elen = pk->key.rsa.elen;
    } else if (pk->key_type == BR_KEYTYPE_EC) {
        ta->pkey.key.ec.curve = pk->key.ec.curve;
        ta->pkey.key.ec.q = arena_put(a, pk->key.ec.q, pk->key.ec.qlen);
        ta->pkey.key.ec.qlen = pk->key.ec.qlen;
    } else {
        return XC_ERR_CORRUPT;
    }
    if (a->overflow || ta->dn.len == 0) {
        return XC_ERR_CORRUPT;
    }
    a->count++;
    return XC_OK;
}

static bool is_cert_name(const char *name)
{
    /* BearSSL leaves the closing dashes on the name when the BEGIN line ends in CRLF
     * (its read-bytes skips the CR without moving back), so trim them here. */
    size_t n = strlen(name);
    while (n > 0 && name[n - 1] == '-') {
        n--;
    }
    return (n == 11 && memcmp(name, "CERTIFICATE", 11) == 0) ||
           (n == 16 && memcmp(name, "X509 CERTIFICATE", 16) == 0);
}

static xc_err parse_pem(xc_tls_anchors *a, der_buf *der, const char *pem, size_t len,
                        size_t *objects)
{
    br_pem_decoder_context pc;
    br_pem_decoder_init(&pc);
    bool in_obj = false;
    size_t pos = 0;
    bool tail_done = false;
    for (;;) {
        size_t pushed = 0;
        if (pos == len && !tail_done) {
            /* A last END line without its newline still ends the object. */
            static const char nl = '\n';
            tail_done = true;
            pushed = br_pem_decoder_push(&pc, &nl, 1);
        } else if (pos < len) {
            pushed = br_pem_decoder_push(&pc, pem + pos, len - pos);
            pos += pushed;
        }
        int ev = br_pem_decoder_event(&pc);
        if (ev == 0 && pushed == 0 && pos < len) {
            return XC_ERR_CORRUPT; /* the decoder stopped without saying why */
        }
        if (ev == BR_PEM_BEGIN_OBJ) {
            ++*objects;
            if (!is_cert_name(br_pem_decoder_name(&pc))) {
                return XC_ERR_CORRUPT;
            }
            der->len = 0;
            der->overflow = false;
            br_pem_decoder_setdest(&pc, der_append, der);
            in_obj = true;
        } else if (ev == BR_PEM_END_OBJ) {
            if (der->overflow) {
                return XC_ERR_LIMIT;
            }
            xc_err err = add_anchor(a, der);
            if (err != XC_OK) {
                return err;
            }
            in_obj = false;
        } else if (ev == BR_PEM_ERROR) {
            return XC_ERR_CORRUPT;
        } else if (pos == len && tail_done) {
            break;
        }
    }
    return in_obj ? XC_ERR_CORRUPT : XC_OK;
}

xc_err xc_tls_anchors_parse(const char *pem, size_t len, xc_tls_anchors **out, size_t *bad_cert)
{
    if (bad_cert != NULL) {
        *bad_cert = 0;
    }
    if (out == NULL || (pem == NULL && len > 0)) {
        return XC_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (len > XC_TLS_MAX_ANCHOR_FILE) {
        return XC_ERR_LIMIT;
    }
    /* Every DN and key comes out of a certificate's DER, which is shorter than its PEM. */
    xc_tls_anchors *a = calloc(1, sizeof *a);
    unsigned char *der_mem = malloc(XC_TLS_MAX_CERT);
    if (a != NULL) {
        a->ta = calloc(XC_TLS_MAX_ANCHORS, sizeof *a->ta);
        a->arena = malloc(len > 0 ? len : 1);
        a->cap = len;
    }
    if (a == NULL || a->ta == NULL || a->arena == NULL || der_mem == NULL) {
        xc_tls_anchors_free(a);
        free(der_mem);
        return XC_ERR_NOMEM;
    }
    der_buf der = {der_mem, 0, false};
    size_t objects = 0;
    xc_err err = parse_pem(a, &der, pem, len, &objects);
    free(der_mem);
    if (err == XC_OK && a->count == 0) {
        err = XC_ERR_CORRUPT;
    }
    if (err != XC_OK) {
        if (bad_cert != NULL && objects > a->count) {
            *bad_cert = objects;
        }
        xc_tls_anchors_free(a);
        return err;
    }
    *out = a;
    return XC_OK;
}

xc_err xc_tls_anchors_builtin(xc_tls_anchors **out)
{
    return xc_tls_anchors_parse((const char *)xc_cacert_pem, xc_cacert_pem_len, out, NULL);
}

const char *xc_tls_builtin_date(void)
{
    return xc_cacert_date;
}

/* ---- Entropy pool (ADR 0012 §6) ------------------------------------------------------- */

struct xc_entropy_pool {
    br_sha256_context hash; /* everything added so far */
    unsigned bits;
    uint64_t outputs;
};

xc_err xc_entropy_pool_create(xc_entropy_pool **out)
{
    if (out == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    *out = calloc(1, sizeof **out);
    if (*out == NULL) {
        return XC_ERR_NOMEM;
    }
    br_sha256_init(&(*out)->hash);
    br_sha256_update(&(*out)->hash, "xcat entropy pool v1", 20);
    return XC_OK;
}

void xc_entropy_pool_destroy(xc_entropy_pool *p)
{
    if (p != NULL) {
        wipe(p, sizeof *p);
        free(p);
    }
}

void xc_entropy_pool_add(xc_entropy_pool *p, const void *data, size_t len, unsigned bits)
{
    if (p == NULL || (data == NULL && len > 0)) {
        return;
    }
    br_sha256_update(&p->hash, data, len);
    p->bits = bits > UINT32_MAX - p->bits ? UINT32_MAX : p->bits + bits;
}

unsigned xc_entropy_pool_bits(const xc_entropy_pool *p)
{
    return p != NULL ? p->bits : 0;
}

static xc_err pool_get(void *ctx, void *buf, size_t len)
{
    xc_entropy_pool *p = ctx;
    if (p == NULL || p->bits < XC_ENTROPY_MIN_BITS) {
        return XC_ERR_TLS_NO_RANDOM;
    }
    unsigned char *dst = buf;
    while (len > 0) {
        /* Output = H(pool || "out" || n); then the pool absorbs it, so the next output and
         * the pool's later state can't be worked out from this one. */
        br_sha256_context h = p->hash;
        unsigned char block[32];
        unsigned char n[8];
        for (int i = 0; i < 8; i++) {
            n[i] = (unsigned char)(p->outputs >> (8 * i));
        }
        p->outputs++;
        br_sha256_update(&h, "out", 3);
        br_sha256_update(&h, n, sizeof n);
        br_sha256_out(&h, block);
        br_sha256_update(&p->hash, "stir", 4);
        br_sha256_update(&p->hash, block, sizeof block);
        size_t take = len < sizeof block ? len : sizeof block;
        memcpy(dst, block, take);
        dst += take;
        len -= take;
        wipe(block, sizeof block);
        wipe(&h, sizeof h);
    }
    return XC_OK;
}

void xc_entropy_pool_source(xc_entropy_pool *p, xc_entropy *e)
{
    if (e != NULL) {
        e->get = pool_get;
        e->ctx = p;
    }
}

/* ---- Cipher speed --------------------------------------------------------------------- */

static uint32_t kb_per_s(size_t bytes, uint64_t ms)
{
    uint64_t kbs = (uint64_t)bytes / (ms > 0 ? ms : 1); /* bytes per ms = kB per s */
    return kbs > UINT32_MAX ? UINT32_MAX : (uint32_t)kbs;
}

xc_err xc_tls_measure_ciphers(const xc_clock *clock, size_t bytes, xc_tls_speed *out)
{
    if (clock == NULL || clock->now_ms == NULL || out == NULL || bytes == 0) {
        return XC_ERR_INVALID_ARG;
    }
    enum { CHUNK = 16384 }; /* one TLS record */
    unsigned char *data = calloc(1, CHUNK);
    if (data == NULL) {
        return XC_ERR_NOMEM;
    }
    static const unsigned char key[32] = {1, 2, 3, 4, 5, 6, 7, 8};
    static const unsigned char iv[12] = {9, 10, 11, 12};
    unsigned char tag[16];

    size_t total = (bytes + CHUNK - 1) / CHUNK * CHUNK; /* whole records */
    uint64_t t0 = clock->now_ms(clock->ctx);
    for (size_t done = 0; done < total; done += CHUNK) {
        br_poly1305_ctmul_run(key, iv, data, CHUNK, NULL, 0, tag, br_chacha20_ct_run, 1);
    }
    uint64_t t1 = clock->now_ms(clock->ctx);
    out->chacha20_poly1305_kbs = kb_per_s(total, t1 - t0);

    br_aes_ct_ctr_keys aes;
    br_gcm_context gcm;
    br_aes_ct_ctr_init(&aes, key, 16);
    br_gcm_init(&gcm, &aes.vtable, br_ghash_ctmul);
    t0 = clock->now_ms(clock->ctx);
    for (size_t done = 0; done < total; done += CHUNK) {
        br_gcm_reset(&gcm, iv, sizeof iv);
        br_gcm_flip(&gcm);
        br_gcm_run(&gcm, 1, data, CHUNK);
        br_gcm_get_tag(&gcm, tag);
    }
    t1 = clock->now_ms(clock->ctx);
    out->aes128_gcm_kbs = kb_per_s(total, t1 - t0);
    free(data);
    return XC_OK;
}

/* ---- The TLS net --------------------------------------------------------------------- */

struct xc_net_conn {
    xc_tls *tls;
};

struct xc_tls {
    xc_tls_config cfg;
    br_ssl_client_context cc;
    br_x509_minimal_context xc;
    xc_net_conn handle;
    xc_net_conn *inner; /* the TCP connection, NULL when closed */
    bool peer_closed;
    int last_error;
    xc_tls_info info;
    unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
};

/* ADR 0012 §2: ECDHE (forward secrecy) and AEAD only; ChaCha20 first (no AES-NI on the Xbox). */
static const struct {
    uint16_t id;
    const char *name;
} suites[] = {
    {BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256, "ECDHE-ECDSA-CHACHA20-POLY1305"},
    {BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256, "ECDHE-RSA-CHACHA20-POLY1305"},
    {BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256, "ECDHE-ECDSA-AES128-GCM-SHA256"},
    {BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256, "ECDHE-RSA-AES128-GCM-SHA256"},
    {BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384, "ECDHE-ECDSA-AES256-GCM-SHA384"},
    {BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384, "ECDHE-RSA-AES256-GCM-SHA384"},
};
#define SUITE_COUNT (sizeof suites / sizeof suites[0])

static xc_err map_error(int e)
{
    if (e == BR_ERR_X509_TIME_UNKNOWN) {
        return XC_ERR_TLS_NO_TIME;
    }
    if (e == BR_ERR_X509_EXPIRED) {
        return XC_ERR_TLS_CERT_TIME;
    }
    if (e > BR_ERR_X509_OK && e < BR_ERR_X509_OK + 32) {
        return XC_ERR_TLS_CERT;
    }
    if (e == BR_ERR_NO_RANDOM) {
        return XC_ERR_TLS_NO_RANDOM;
    }
    return XC_ERR_TLS;
}

/* The engine failed or was closed: XC_OK for a clean close_notify, else the mapped error. */
static xc_err closed_error(xc_tls *t)
{
    int e = br_ssl_engine_last_error(&t->cc.eng);
    if (e == BR_ERR_OK) {
        return XC_OK;
    }
    t->last_error = e;
    return map_error(e);
}

static xc_err send_records(xc_tls *t, uint32_t timeout_ms)
{
    size_t len = 0;
    unsigned char *buf = br_ssl_engine_sendrec_buf(&t->cc.eng, &len);
    const xc_net *tcp = t->cfg.tcp;
    xc_err err = tcp->send(tcp->ctx, t->inner, buf, len, timeout_ms);
    if (err == XC_OK) {
        br_ssl_engine_sendrec_ack(&t->cc.eng, len);
    }
    return err;
}

/* Reads more record bytes. Sets peer_closed if the TCP connection ended. */
static xc_err recv_records(xc_tls *t, uint32_t timeout_ms)
{
    size_t len = 0;
    unsigned char *buf = br_ssl_engine_recvrec_buf(&t->cc.eng, &len);
    const xc_net *tcp = t->cfg.tcp;
    size_t got = 0;
    xc_err err = tcp->recv(tcp->ctx, t->inner, buf, len, timeout_ms, &got);
    if (err != XC_OK) {
        return err;
    }
    if (got == 0) {
        t->peer_closed = true;
        return XC_OK;
    }
    br_ssl_engine_recvrec_ack(&t->cc.eng, got);
    return XC_OK;
}

static xc_err handshake(xc_tls *t, uint32_t timeout_ms)
{
    for (;;) {
        unsigned st = br_ssl_engine_current_state(&t->cc.eng);
        if (st & BR_SSL_CLOSED) {
            xc_err err = closed_error(t);
            return err != XC_OK ? err : XC_ERR_NET_IO;
        }
        xc_err err = XC_OK;
        if (st & BR_SSL_SENDREC) {
            err = send_records(t, timeout_ms);
        } else if (st & (BR_SSL_SENDAPP | BR_SSL_RECVAPP)) {
            return XC_OK;
        } else if (st & BR_SSL_RECVREC) {
            err = recv_records(t, timeout_ms);
            if (err == XC_OK && t->peer_closed) {
                err = XC_ERR_NET_IO; /* the server hung up mid-handshake */
            }
        } else {
            err = XC_ERR_TLS;
        }
        if (err != XC_OK) {
            /* A rejected certificate queues an alert first; if sending it fails, the
             * certificate is still the reason. */
            int e = br_ssl_engine_last_error(&t->cc.eng);
            if (e != BR_ERR_OK) {
                t->last_error = e;
                return map_error(e);
            }
            return err;
        }
    }
}

static void close_inner(xc_tls *t)
{
    if (t->inner != NULL) {
        t->cfg.tcp->close(t->cfg.tcp->ctx, t->inner);
        t->inner = NULL;
    }
}

static void record_info(xc_tls *t)
{
    br_ssl_session_parameters sp;
    br_ssl_engine_get_session_parameters(&t->cc.eng, &sp);
    t->info.version = sp.version;
    t->info.cipher_suite = sp.cipher_suite;
    t->info.suite_name = "?";
    for (size_t i = 0; i < SUITE_COUNT; i++) {
        if (suites[i].id == sp.cipher_suite) {
            t->info.suite_name = suites[i].name;
        }
    }
    wipe(&sp, sizeof sp); /* it holds the master secret */
}

static xc_err tls_connect(void *ctx, const char *host, uint16_t port, uint32_t timeout_ms,
                          xc_net_conn **out)
{
    xc_tls *t = ctx;
    if (t == NULL || host == NULL || host[0] == '\0' || out == NULL || t->inner != NULL) {
        return XC_ERR_INVALID_ARG;
    }
    *out = NULL;
    t->last_error = 0;
    /* Time and randomness first, so nothing goes on the wire without them. */
    int64_t now = 0;
    xc_err err = t->cfg.wallclock->now(t->cfg.wallclock->ctx, &now);
    if (err != XC_OK || now < 0) {
        return XC_ERR_TLS_NO_TIME;
    }
    unsigned char seed[XC_TLS_SEED_BYTES];
    err = t->cfg.entropy->get(t->cfg.entropy->ctx, seed, sizeof seed);
    if (err != XC_OK) {
        wipe(seed, sizeof seed);
        return XC_ERR_TLS_NO_RANDOM;
    }
    br_ssl_engine_inject_entropy(&t->cc.eng, seed, sizeof seed);
    wipe(seed, sizeof seed);
    /* BearSSL counts days from 0000-01-01 (proleptic Gregorian); 1970-01-01 is day 719528. */
    br_x509_minimal_set_time(&t->xc, (uint32_t)(now / 86400) + 719528U, (uint32_t)(now % 86400));

    err = t->cfg.tcp->connect(t->cfg.tcp->ctx, host, port, timeout_ms, &t->inner);
    if (err != XC_OK) {
        t->inner = NULL;
        return err;
    }
    t->peer_closed = false;
    memset(&t->info, 0, sizeof t->info);
    /* The host name is both the SNI and what the certificate must name. No resumption:
     * a session from one host must never skip the certificate check for another. */
    if (!br_ssl_client_reset(&t->cc, host, 0)) {
        err = closed_error(t);
        close_inner(t);
        return err != XC_OK ? err : XC_ERR_TLS;
    }
    err = handshake(t, timeout_ms);
    if (err != XC_OK) {
        close_inner(t);
        return err;
    }
    record_info(t);
    *out = &t->handle;
    return XC_OK;
}

static xc_err tls_send(void *ctx, xc_net_conn *conn, const void *buf, size_t len,
                       uint32_t timeout_ms)
{
    xc_tls *t = ctx;
    if (t == NULL || conn != &t->handle || t->inner == NULL || (buf == NULL && len > 0)) {
        return XC_ERR_INVALID_ARG;
    }
    const unsigned char *p = buf;
    while (len > 0) {
        unsigned st = br_ssl_engine_current_state(&t->cc.eng);
        if (st & BR_SSL_CLOSED) {
            xc_err err = closed_error(t);
            return err != XC_OK ? err : XC_ERR_NET_IO;
        }
        xc_err err = XC_OK;
        if (st & BR_SSL_SENDREC) {
            err = send_records(t, timeout_ms);
        } else if (st & BR_SSL_SENDAPP) {
            size_t room = 0;
            unsigned char *dst = br_ssl_engine_sendapp_buf(&t->cc.eng, &room);
            size_t n = len < room ? len : room;
            memcpy(dst, p, n);
            br_ssl_engine_sendapp_ack(&t->cc.eng, n);
            p += n;
            len -= n;
        } else if ((st & BR_SSL_RECVREC) && !t->peer_closed) {
            err = recv_records(t, timeout_ms);
        } else {
            err = XC_ERR_NET_IO;
        }
        if (err != XC_OK) {
            return err;
        }
    }
    br_ssl_engine_flush(&t->cc.eng, 0);
    while (br_ssl_engine_current_state(&t->cc.eng) & BR_SSL_SENDREC) {
        xc_err err = send_records(t, timeout_ms);
        if (err != XC_OK) {
            return err;
        }
    }
    return XC_OK;
}

static xc_err tls_recv(void *ctx, xc_net_conn *conn, void *buf, size_t cap, uint32_t timeout_ms,
                       size_t *got)
{
    xc_tls *t = ctx;
    if (t == NULL || conn != &t->handle || t->inner == NULL || buf == NULL || cap == 0 ||
        got == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    *got = 0;
    for (;;) {
        unsigned st = br_ssl_engine_current_state(&t->cc.eng);
        if (st & BR_SSL_CLOSED) {
            return closed_error(t); /* XC_OK with *got == 0 after close_notify */
        }
        xc_err err = XC_OK;
        if (st & BR_SSL_RECVAPP) {
            size_t avail = 0;
            unsigned char *src = br_ssl_engine_recvapp_buf(&t->cc.eng, &avail);
            size_t n = avail < cap ? avail : cap;
            memcpy(buf, src, n);
            br_ssl_engine_recvapp_ack(&t->cc.eng, n);
            *got = n;
            return XC_OK;
        }
        if (st & BR_SSL_SENDREC) {
            err = send_records(t, timeout_ms);
        } else if (st & BR_SSL_RECVREC) {
            if (t->peer_closed) {
                return XC_OK; /* TCP ended without close_notify; the caller sees the end */
            }
            err = recv_records(t, timeout_ms);
        } else {
            err = XC_ERR_TLS;
        }
        if (err != XC_OK) {
            return err;
        }
    }
}

static void tls_close(void *ctx, xc_net_conn *conn)
{
    xc_tls *t = ctx;
    if (t != NULL && conn == &t->handle) {
        close_inner(t);
    }
}

xc_err xc_tls_create(const xc_tls_config *cfg, xc_tls **out, xc_net *net)
{
    if (cfg == NULL || out == NULL || net == NULL || cfg->tcp == NULL || cfg->anchors == NULL ||
        cfg->entropy == NULL || cfg->entropy->get == NULL || cfg->wallclock == NULL ||
        cfg->wallclock->now == NULL || cfg->anchors->count == 0) {
        return XC_ERR_INVALID_ARG;
    }
    *out = NULL;
    xc_tls *t = calloc(1, sizeof *t);
    if (t == NULL) {
        return XC_ERR_NOMEM;
    }
    t->cfg = *cfg;
    t->handle.tls = t;
    br_ssl_client_init_full(&t->cc, &t->xc, cfg->anchors->ta, cfg->anchors->count);
    br_ssl_engine_set_versions(&t->cc.eng, BR_TLS12, BR_TLS12);
    uint16_t ids[SUITE_COUNT];
    for (size_t i = 0; i < SUITE_COUNT; i++) {
        ids[i] = suites[i].id;
    }
    br_ssl_engine_set_suites(&t->cc.eng, ids, SUITE_COUNT);
    br_ssl_engine_set_buffer(&t->cc.eng, t->iobuf, sizeof t->iobuf, 1);

    net->connect = tls_connect;
    net->send = tls_send;
    net->recv = tls_recv;
    net->close = tls_close;
    net->ctx = t;
    *out = t;
    return XC_OK;
}

void xc_tls_destroy(xc_tls *t)
{
    if (t != NULL) {
        close_inner(t);
        wipe(t, sizeof *t);
        free(t);
    }
}

void xc_tls_last_info(const xc_tls *t, xc_tls_info *info)
{
    if (info == NULL) {
        return;
    }
    if (t == NULL) {
        memset(info, 0, sizeof *info);
        return;
    }
    *info = t->info;
}

int xc_tls_last_error(const xc_tls *t)
{
    return t != NULL ? t->last_error : 0;
}
