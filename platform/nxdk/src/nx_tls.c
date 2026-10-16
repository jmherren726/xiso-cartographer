/*
 * What TLS needs from the Xbox (xcat/tls.h, ADR 0012): the time for certificate checks, and
 * seed material for the random generator, which the console has no source of its own for.
 */
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include <lwip/netif.h>

#include "xcat/nxdk.h"

#ifndef XC_BUILD_UNIX_TIME
#error "XC_BUILD_UNIX_TIME must come from the Makefile (app/tls.mk): the clock's floor"
#endif

extern struct netif *g_pnetif; /* nxdk/net.c */

#define SEED_DIR "E:\\Apps\\xcat"
#define SEED_FILE "E:\\Apps\\xcat\\random-seed"
#define SEED_BYTES 32U
#define JITTER_SAMPLES 256U

/* --- Time (ADR 0012 §5) ----------------------------------------------------------------- */

int64_t xc_nx_console_time(void)
{
    LARGE_INTEGER t;
    KeQuerySystemTime(&t); /* UTC, 100 ns units since 1601-01-01 */
    return t.QuadPart / 10000000LL - 11644473600LL;
}

int64_t xc_nx_build_time(void)
{
    return (int64_t)XC_BUILD_UNIX_TIME;
}

/* The console's clock, unless it's earlier than this XBE's build: then it was reset (a flat
 * clock capacitor sends it back to 2001) and nothing it says can be trusted. */
static xc_err nx_now(void *ctx, int64_t *unix_s)
{
    (void)ctx;
    int64_t now = xc_nx_console_time();
    if (now < xc_nx_build_time()) {
        return XC_ERR_TLS_NO_TIME;
    }
    *unix_s = now;
    return XC_OK;
}

void xc_nx_wallclock(xc_wallclock *w)
{
    w->now = nx_now;
    w->ctx = NULL;
}

/* --- Entropy (ADR 0012 §6) -------------------------------------------------------------- */

static uint64_t tsc(void)
{
    return __builtin_ia32_rdtsc();
}

static bool read_seed(unsigned char *buf, size_t len)
{
    HANDLE h = CreateFileA(SEED_FILE, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD got = 0;
    bool ok = ReadFile(h, buf, (DWORD)len, &got, NULL) && got == len;
    CloseHandle(h);
    return ok;
}

static bool write_seed(const unsigned char *buf, size_t len)
{
    if (!xc_nx_mkdirs(SEED_DIR)) {
        return false;
    }
    HANDLE h =
        CreateFileA(SEED_FILE, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD put = 0;
    bool ok = WriteFile(h, buf, (DWORD)len, &put, NULL) && put == len;
    IO_STATUS_BLOCK io;
    ok = ok && NT_SUCCESS(NtFlushBuffersFile(h, &io));
    CloseHandle(h);
    return ok;
}

xc_err xc_nx_entropy_gather(xc_entropy_pool *pool, xc_nx_entropy_stats *st)
{
    if (pool == NULL || st == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    memset(st, 0, sizeof *st);
    /* Mixed in, credited nothing: unique to this console or this boot, but guessable. */
    unsigned char seed[SEED_BYTES];
    st->seed_read = read_seed(seed, sizeof seed);
    if (st->seed_read) {
        xc_entropy_pool_add(pool, seed, sizeof seed, 0);
    }
    int64_t now = xc_nx_console_time();
    xc_entropy_pool_add(pool, &now, sizeof now, 0);
    LARGE_INTEGER pc;
    QueryPerformanceCounter(&pc);
    xc_entropy_pool_add(pool, &pc, sizeof pc, 0);
    if (g_pnetif != NULL) {
        xc_entropy_pool_add(pool, g_pnetif->hwaddr, g_pnetif->hwaddr_len, 0);
    }
    /*
     * Credited: how many CPU cycles a 1 ms sleep takes. The wake-up comes from the timer
     * interrupt, and its exact cycle varies with interrupt latency, cache state and the
     * other interrupts (network, USB, disk) the console takes meanwhile. Each sample counts
     * as one bit, and only if it differs from the one before.
     */
    bool seen[256];
    memset(seen, 0, sizeof seen);
    uint64_t prev = 0;
    for (unsigned i = 0; i < JITTER_SAMPLES; i++) {
        uint64_t t0 = tsc();
        Sleep(1);
        uint64_t d = tsc() - t0;
        xc_entropy_pool_add(pool, &d, sizeof d, d != prev ? 1U : 0U);
        if (!seen[d & 0xFFU]) {
            seen[d & 0xFFU] = true;
            st->distinct_low_bytes++;
        }
        if (i == 0 || d < st->min_cycles) {
            st->min_cycles = d;
        }
        if (d > st->max_cycles) {
            st->max_cycles = d;
        }
        prev = d;
        st->samples++;
    }
    st->bits = xc_entropy_pool_bits(pool);
    if (st->bits < XC_ENTROPY_MIN_BITS) {
        return XC_ERR_TLS_NO_RANDOM;
    }
    /* A fresh seed for the next start, from the pool (never the same bytes twice). */
    xc_entropy e;
    xc_entropy_pool_source(pool, &e);
    if (e.get(e.ctx, seed, sizeof seed) == XC_OK) {
        st->seed_written = write_seed(seed, sizeof seed);
    }
    memset(seed, 0, sizeof seed);
    return XC_OK;
}
