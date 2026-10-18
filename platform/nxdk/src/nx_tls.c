/*
 * What TLS needs from the Xbox (xcat/tls.h, ADR 0012): the time for certificate checks, and
 * seed material for the random generator, which the console has no source of its own for;
 * and the HTTPS setup an XBE does once (xc_nx_https_start).
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include <lwip/netif.h>

#include "xcat/nxdk.h"
#include "xcat/sntp.h"

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

/* --- HTTPS for an XBE: randomness, time, trust, the TLS client ------------------------- */

#define CA_FILE "E:\\Apps\\xcat\\cacert.pem"
#define NTP_FILE "E:\\Apps\\xcat\\ntp.txt"

void xc_nx_utc_date(char *buf, size_t size, int64_t t)
{
    int64_t days = t >= 0 ? t / 86400 : (t - 86399) / 86400;
    int64_t secs = t - days * 86400;
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1;
    int64_t m = mp < 10 ? mp + 3 : mp - 9;
    int64_t y = yoe + era * 400 + (m <= 2 ? 1 : 0);
    (void)snprintf(buf, size, "%04lld-%02lld-%02lld %02lld:%02lld:%02lld UTC", (long long)y,
                   (long long)m, (long long)d, (long long)(secs / 3600),
                   (long long)(secs / 60 % 60), (long long)(secs % 60));
}

/* Reads a small file whole, NUL-terminated; NULL if it doesn't exist or can't be read. */
static char *read_small(const char *path, size_t max, size_t *len)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return NULL;
    }
    char *data = malloc(max + 2);
    DWORD got = 0;
    bool ok = data != NULL && ReadFile(h, data, (DWORD)(max + 1), &got, NULL);
    CloseHandle(h);
    if (!ok) {
        free(data);
        return NULL;
    }
    data[got] = '\0';
    *len = got; /* one byte more than allowed is caught by the caller's limit */
    return data;
}

const char *xc_nx_ntp_setting(char *buf, size_t cap)
{
    size_t len = 0;
    char *data = read_small(NTP_FILE, 512, &len);
    if (data == NULL) {
        return XC_SNTP_DEFAULT_SERVER;
    }
    const char *server = XC_SNTP_DEFAULT_SERVER;
    char *p = data;
    while (*p != '\0') {
        char *line = p;
        size_t n = strcspn(line, "\r\n");
        p = line + n;
        if (*p != '\0') {
            *p++ = '\0';
        }
        while (*line == ' ' || *line == '\t') {
            line++;
            n--;
        }
        while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t')) {
            line[--n] = '\0';
        }
        if (n == 0 || line[0] == '#') {
            continue;
        }
        if (strcmp(line, "off") == 0) {
            server = NULL;
        } else if (n < cap) {
            memcpy(buf, line, n + 1);
            server = buf;
        }
        break;
    }
    free(data);
    return server;
}

__attribute__((format(printf, 2, 3))) static void say(const xc_nx_https_opts *o, const char *fmt,
                                                      ...)
{
    if (o->say == NULL) {
        return;
    }
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n >= 0) {
        o->say(line);
    }
}

xc_err xc_nx_https_start(const xc_nx_https_opts *o, xc_net *tls_net, xc_tls **tls)
{
    /* One set per program: what the TLS client points to must outlive it. */
    static xc_entropy_pool *pool;
    static xc_entropy entropy;
    static xc_wallclock console;
    static xc_wallclock wallclock;
    static xc_time_source time_src;
    static xc_udp udp;
    static xc_tls_anchors *anchors;

    char date[40];
    char floor_date[40];
    xc_nx_utc_date(date, sizeof date, xc_nx_console_time());
    xc_nx_utc_date(floor_date, sizeof floor_date, xc_nx_build_time());
    say(o, "Console clock %s (this build: %s)\n", date, floor_date);

    /* ADR 0012 §6 */
    xc_nx_entropy_stats st;
    memset(&st, 0, sizeof st);
    xc_err err = pool != NULL ? XC_OK : xc_entropy_pool_create(&pool);
    if (err == XC_OK) {
        err = xc_nx_entropy_gather(pool, &st);
        say(o,
            "Randomness: %u timer samples, %u bits credited, %u of 256 low bytes seen, "
            "%llu-%llu cycles; seed file %s, %s\n",
            st.samples, st.bits, st.distinct_low_bytes, (unsigned long long)st.min_cycles,
            (unsigned long long)st.max_cycles, st.seed_read ? "read" : "none yet",
            st.seed_written ? "renewed" : "NOT written");
    }
    if (err != XC_OK) {
        say(o, "Not enough randomness for TLS: %s\n", xc_err_str(err));
        return err;
    }
    xc_entropy_pool_source(pool, &entropy);

    /* docs/formats/sntp.md §6, ADR 0012 §5. The nonce comes from the pool, so after it. */
    const xc_clock *clock = o->clock;
    xc_nx_wallclock(&console);
    xc_nx_udp(&udp);
    err = xc_time_choose(&time_src, &udp, clock, &entropy, o->ntp_server, XC_SNTP_PORT,
                         XC_SNTP_DEFAULT_TIMEOUT_MS, &console, xc_nx_build_time());
    if (time_src.kind == XC_TIME_NTP) {
        int64_t ntp_ms =
            time_src.ntp.unix_ms + (int64_t)(clock->now_ms(clock->ctx) - time_src.ntp.mono_ms);
        int64_t off_s = xc_nx_console_time() - ntp_ms / 1000;
        xc_nx_utc_date(date, sizeof date, ntp_ms / 1000);
        say(o,
            "Time from NTP (%s): %s, stratum %u, round trip %lu ms, %u request%s; the console's "
            "clock is %+lld s off\n",
            o->ntp_server, date, (unsigned)time_src.ntp.stratum, (unsigned long)time_src.ntp.rtt_ms,
            time_src.ntp.attempts, time_src.ntp.attempts == 1 ? "" : "s", (long long)off_s);
    } else {
        if (o->ntp_server == NULL) {
            say(o, "NTP is off.\n");
        } else {
            const xc_sntp_failure *f = &time_src.failure;
            say(o, "NTP (%s) failed after %u request%s: %s%s%s%s%s\n", o->ntp_server, f->attempts,
                f->attempts == 1 ? "" : "s", xc_err_str(f->err), f->why != NULL ? ": " : "",
                f->why != NULL ? f->why : "", f->kiss[0] != '\0' ? " " : "", f->kiss);
        }
        if (err != XC_OK) {
            say(o,
                "%s. The console's clock is earlier than this XBE's build, so it was reset: set "
                "the date and time in the dashboard's settings.\n",
                xc_err_str(err));
            return err;
        }
        say(o, "Time from the console's clock (not earlier than this build)\n");
    }
    xc_time_wallclock(&time_src, &wallclock);

    /* ADR 0012 §3 and §4: E:\Apps\xcat\cacert.pem replaces the built-in bundle. */
    if (anchors == NULL) {
        size_t len = 0;
        char *pem = read_small(CA_FILE, XC_TLS_MAX_ANCHOR_FILE, &len);
        size_t bad = 0;
        if (pem != NULL) {
            err = xc_tls_anchors_parse(pem, len, &anchors, &bad);
            free(pem);
            if (err != XC_OK) {
                say(o, "%s can't be used (%s, certificate %u). Fix or delete it.\n", CA_FILE,
                    xc_err_str(err), (unsigned)bad);
                return err;
            }
            say(o, "Trusting the %u certificates in %s\n", (unsigned)xc_tls_anchors_count(anchors),
                CA_FILE);
        } else {
            err = xc_tls_anchors_builtin(&anchors);
            if (err != XC_OK) {
                say(o, "Built-in CA bundle: %s\n", xc_err_str(err));
                return err;
            }
            say(o, "Trusting the %u built-in roots (Mozilla, %s)\n",
                (unsigned)xc_tls_anchors_count(anchors), xc_tls_builtin_date());
        }
    }
    xc_tls_config tc = {o->tcp, anchors, &entropy, &wallclock};
    err = xc_tls_create(&tc, tls, tls_net);
    if (err != XC_OK) {
        say(o, "TLS: %s\n", xc_err_str(err));
    }
    return err;
}
