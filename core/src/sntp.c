/*
 * SNTP client: docs/formats/sntp.md §2 (request), §3 (timestamps), §4 (checks on the answer),
 * §5 (result), §6 (attempts and the fallback to the console's clock, ADR 0012 §5).
 */
#include "xcat/sntp.h"

#include <string.h>

#define NTP_UNIX_OFFSET 2208988800LL /* seconds from 1900-01-01 to 1970-01-01 */
#define ANSWER_CAP 128U              /* §4.1: anything after byte 48 is ignored */

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24U) | ((uint32_t)p[1] << 16U) | ((uint32_t)p[2] << 8U) |
           (uint32_t)p[3];
}

void xc_sntp_request(uint8_t out[XC_SNTP_PACKET_BYTES], const uint8_t nonce[XC_SNTP_NONCE_BYTES])
{
    memset(out, 0, XC_SNTP_PACKET_BYTES);
    out[0] = 0x23U; /* LI 0, VN 4, Mode 3 (client) */
    memcpy(out + 40, nonce, XC_SNTP_NONCE_BYTES);
}

/* §3: Unix milliseconds of the timestamp at `p`, or 0 when it is all zeros ("not set"). */
static int64_t ts_ms(const uint8_t *p)
{
    uint32_t secs = be32(p);
    uint32_t frac = be32(p + 4);
    if (secs == 0 && frac == 0) {
        return 0;
    }
    int64_t unix_s = (int64_t)secs - NTP_UNIX_OFFSET;
    if ((secs & 0x80000000U) == 0) {
        unix_s += 4294967296LL; /* era 1: 2036-2104 */
    }
    return unix_s * 1000 + (int64_t)(((uint64_t)frac * 1000U) >> 32U);
}

static xc_err refuse(const char **why, const char *reason)
{
    *why = reason;
    return XC_ERR_NTP;
}

xc_err xc_sntp_parse(const uint8_t *buf, size_t len, const uint8_t nonce[XC_SNTP_NONCE_BYTES],
                     xc_sntp_reply *out, const char **why, char kiss[5])
{
    if (buf == NULL || nonce == NULL || out == NULL || why == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    *why = NULL;
    if (kiss != NULL) {
        kiss[0] = '\0';
    }
    if (len < XC_SNTP_PACKET_BYTES) {
        return refuse(why, "answer too short");
    }
    unsigned li = (unsigned)buf[0] >> 6U;
    unsigned vn = ((unsigned)buf[0] >> 3U) & 7U;
    unsigned mode = (unsigned)buf[0] & 7U;
    uint8_t stratum = buf[1];
    if (mode != 4U) {
        return refuse(why, "not a server answer");
    }
    if (vn != 3U && vn != 4U) {
        return refuse(why, "unknown NTP version");
    }
    if (li == 3U) {
        return refuse(why, "server not synchronized");
    }
    if (stratum == 0) {
        if (kiss != NULL) {
            for (size_t i = 0; i < 4; i++) {
                char c = (char)buf[12 + i];
                kiss[i] = (c >= 0x20 && c < 0x7F) ? c : '?';
            }
            kiss[4] = '\0';
        }
        return refuse(why, "kiss-o'-death");
    }
    if (stratum >= 16U) {
        return refuse(why, "server not synchronized");
    }
    if (memcmp(buf + 24, nonce, XC_SNTP_NONCE_BYTES) != 0) {
        return refuse(why, "answer to a different request");
    }
    int64_t transmit = ts_ms(buf + 40);
    if (transmit == 0) {
        return refuse(why, "no transmit time");
    }
    out->transmit_ms = transmit;
    out->receive_ms = ts_ms(buf + 32);
    out->stratum = stratum;
    out->version = (uint8_t)vn;
    return XC_OK;
}

int64_t xc_sntp_arrival_ms(const xc_sntp_reply *reply, uint32_t rtt_ms)
{
    int64_t server = 0;
    if (reply->receive_ms != 0 && reply->receive_ms <= reply->transmit_ms) {
        server = reply->transmit_ms - reply->receive_ms;
    }
    int64_t rtt = (int64_t)rtt_ms;
    return reply->transmit_ms + (rtt > server ? (rtt - server) / 2 : 0);
}

static void fail_set(xc_sntp_failure *fail, xc_err err, const char *why, const char *kiss,
                     unsigned attempts)
{
    if (fail == NULL) {
        return;
    }
    fail->err = err;
    fail->why = why;
    size_t n = strlen(kiss);
    n = n > 4 ? 4 : n;
    (void)memcpy(fail->kiss, kiss, n);
    fail->kiss[n] = '\0';
    fail->attempts = attempts;
}

xc_err xc_sntp_query(const xc_udp *udp, const xc_clock *clock, const xc_entropy *entropy,
                     const char *server, uint16_t port, uint32_t timeout_ms, int64_t floor_unix_s,
                     xc_sntp_result *out, xc_sntp_failure *fail)
{
    if (udp == NULL || udp->exchange == NULL || clock == NULL || clock->now_ms == NULL ||
        entropy == NULL || entropy->get == NULL || server == NULL || out == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    xc_err err = XC_ERR_NET_TIMEOUT;
    const char *why = NULL;
    char kiss[5] = "";
    unsigned attempt = 0;
    while (attempt < XC_SNTP_ATTEMPTS) {
        attempt++;
        uint8_t nonce[XC_SNTP_NONCE_BYTES];
        if (entropy->get(entropy->ctx, nonce, sizeof nonce) != XC_OK) {
            fail_set(fail, XC_ERR_TLS_NO_RANDOM, NULL, "", attempt - 1U);
            return XC_ERR_TLS_NO_RANDOM;
        }
        uint8_t req[XC_SNTP_PACKET_BYTES];
        xc_sntp_request(req, nonce);
        uint8_t ans[ANSWER_CAP];
        size_t got = 0;
        uint32_t rtt = 0;
        err = udp->exchange(udp->ctx, server, port, req, sizeof req, ans, sizeof ans, timeout_ms,
                            &got, &rtt);
        uint64_t mono = clock->now_ms(clock->ctx);
        why = NULL;
        if (err == XC_ERR_NET_DNS) {
            break; /* §6: asking again won't resolve it */
        }
        if (err != XC_OK) {
            continue;
        }
        xc_sntp_reply reply;
        err = xc_sntp_parse(ans, got > sizeof ans ? sizeof ans : got, nonce, &reply, &why, kiss);
        if (err != XC_OK) {
            if (kiss[0] != '\0') {
                break; /* §6: the server asked us to stop */
            }
            continue;
        }
        int64_t at = xc_sntp_arrival_ms(&reply, rtt);
        if (at / 1000 < floor_unix_s) {
            err = XC_ERR_NTP;
            why = "time earlier than this program's build";
            break;
        }
        out->unix_ms = at;
        out->mono_ms = mono;
        out->rtt_ms = rtt;
        out->stratum = reply.stratum;
        out->attempts = attempt;
        return XC_OK;
    }
    fail_set(fail, err, err == XC_ERR_NTP ? why : NULL, kiss, attempt);
    return err;
}

/* --- time source (ADR 0012 §5) --------------------------------------------------------- */

xc_err xc_time_choose(xc_time_source *t, const xc_udp *udp, const xc_clock *clock,
                      const xc_entropy *entropy, const char *server, uint16_t port,
                      uint32_t timeout_ms, const xc_wallclock *fallback, int64_t floor_unix_s)
{
    if (t == NULL || clock == NULL || clock->now_ms == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    memset(t, 0, sizeof *t);
    t->kind = XC_TIME_NONE;
    t->clock = clock;
    t->fallback = fallback;
    t->floor_unix_s = floor_unix_s;
    if (server != NULL) {
        t->ntp_asked = true;
        xc_err err = xc_sntp_query(udp, clock, entropy, server, port, timeout_ms, floor_unix_s,
                                   &t->ntp, &t->failure);
        if (err == XC_OK) {
            t->kind = XC_TIME_NTP;
            return XC_OK;
        }
        if (err == XC_ERR_INVALID_ARG) {
            return err;
        }
    }
    int64_t now = 0;
    if (fallback != NULL && fallback->now != NULL && fallback->now(fallback->ctx, &now) == XC_OK &&
        now >= floor_unix_s) {
        t->kind = XC_TIME_FALLBACK;
        return XC_OK;
    }
    return XC_ERR_TLS_NO_TIME;
}

static xc_err source_now(void *ctx, int64_t *unix_s)
{
    const xc_time_source *t = ctx;
    if (t->kind == XC_TIME_NTP) {
        uint64_t mono = t->clock->now_ms(t->clock->ctx);
        int64_t ms = t->ntp.unix_ms + (int64_t)(mono - t->ntp.mono_ms);
        *unix_s = ms / 1000;
        return XC_OK;
    }
    if (t->kind == XC_TIME_FALLBACK) {
        int64_t now = 0;
        xc_err err = t->fallback->now(t->fallback->ctx, &now);
        if (err != XC_OK || now < t->floor_unix_s) {
            return XC_ERR_TLS_NO_TIME;
        }
        *unix_s = now;
        return XC_OK;
    }
    return XC_ERR_TLS_NO_TIME;
}

void xc_time_wallclock(xc_time_source *t, xc_wallclock *out)
{
    out->now = source_now;
    out->ctx = t;
}
