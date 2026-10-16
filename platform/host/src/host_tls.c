/*
 * What TLS needs from a PC (xcat/tls.h, ADR 0012 §5-§6): the operating system's random
 * generator and its clock.
 */

#include <stdio.h>
#include <time.h>

#include "xcat/host.h"

#ifdef _WIN32
#include <windows.h>
/* windows.h first: bcrypt.h needs its types. */
#include <bcrypt.h>
#endif

static xc_err host_random(void *ctx, void *buf, size_t len)
{
    (void)ctx;
#ifdef _WIN32
    if (len > 0xFFFFFFFFU ||
        !BCRYPT_SUCCESS(BCryptGenRandom(NULL, buf, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
        return XC_ERR_TLS_NO_RANDOM;
    }
    return XC_OK;
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if (f == NULL) {
        return XC_ERR_TLS_NO_RANDOM;
    }
    size_t got = fread(buf, 1, len, f);
    (void)fclose(f);
    return got == len ? XC_OK : XC_ERR_TLS_NO_RANDOM;
#endif
}

void xc_host_entropy(xc_entropy *e)
{
    e->get = host_random;
    e->ctx = NULL;
}

static xc_err host_now(void *ctx, int64_t *unix_s)
{
    (void)ctx;
    time_t t = time(NULL);
    if (t == (time_t)-1) {
        return XC_ERR_TLS_NO_TIME;
    }
    *unix_s = (int64_t)t;
    return XC_OK;
}

void xc_host_wallclock(xc_wallclock *w)
{
    w->now = host_now;
    w->ctx = NULL;
}
