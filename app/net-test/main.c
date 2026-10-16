/*
 * Network test XBE (M3, HTTPS since M5): downloads one file with the core client and reports
 * MB/s, every retry (e.g. while the cable is pulled) and the CRC-32. For https URLs it also
 * measures how fast this CPU runs the TLS ciphers with no network involved, and says which
 * TLS version and cipher suite the server chose (the M5 go/no-go, ADR 0005).
 *
 * Reads E:\xcat\net-test.txt, one "key=value" per line:
 *
 *   url=http://192.168.1.10:8080/xcat-test-1073741824.bin   (required; http or https)
 *   size=1073741824     expected size in bytes (optional)
 *   crc32=0123abcd      expected CRC-32 (optional; tools/testserver/make_test_file.py prints both)
 *   limit=268435456     download only the first N bytes (optional; e.g. part of a large file)
 *   save=yes            also write the file to E:\xcat\net-test.bin (network + disk speed)
 *   attempts=40         consecutive failures before giving up (default 40, about 30 minutes)
 *
 * For https, certificates are checked against the built-in Mozilla roots, or against
 * E:\Apps\xcat\cacert.pem instead if that file exists (ADR 0012 §4), and against the
 * console's clock, which must be set (NTP comes later in M5). Results go on screen and are
 * appended to E:\xcat\net-test.log. See docs/testing.md ("M5: TLS speed on the console").
 */
#include <hal/debug.h>
#include <hal/video.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include "xcat/crc32.h"
#include "xcat/http_client.h"
#include "xcat/nxdk.h"
#include "xcat/tls.h"
#include "xcat/version.h"

#define CONFIG "E:\\xcat\\net-test.txt"
#define LOG "E:\\xcat\\net-test.log"
#define SAVE "E:\\xcat\\net-test.bin"
#define CA_FILE "E:\\Apps\\xcat\\cacert.pem"
#define RECV_BUF_SIZE ((size_t)256 * 1024)
#define WRITE_BUF_SIZE ((size_t)1024 * 1024) /* large writes: FATX is slow with small ones */
#define DEFAULT_ATTEMPTS 40U
#define REPORT_EVERY_MS 5000U
#define LOG_MAX_BYTES (1024U * 1024U) /* the log is appended to; past this it starts over */
#define CIPHER_TEST_BYTES ((size_t)8 * 1024 * 1024)

static HANDLE log_file = INVALID_HANDLE_VALUE;

/* Prints to the screen and the log. */
__attribute__((format(printf, 1, 2))) static void out(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if ((size_t)n >= sizeof line) {
        n = (int)sizeof line - 1;
    }
    debugPrint("%s", line);
    if (log_file != INVALID_HANDLE_VALUE) {
        DWORD put = 0;
        (void)WriteFile(log_file, line, (DWORD)n, &put, NULL);
        /* To the disk now: the test usually ends with the power switch, which would drop
         * whatever the FATX cache still held (and with it the end of an unfinished run). */
        IO_STATUS_BLOCK io;
        (void)NtFlushBuffersFile(log_file, &io);
    }
}

static void halt(void)
{
    if (log_file != INVALID_HANDLE_VALUE) {
        CloseHandle(log_file);
        log_file = INVALID_HANDLE_VALUE;
    }
    for (;;) {
        Sleep(1000);
    }
}

/* --- config --------------------------------------------------------------------------- */

typedef struct config {
    char url[XC_URL_MAX + 1];
    bool has_size;
    uint64_t size;
    bool has_crc;
    uint32_t crc;
    uint64_t limit; /* 0: the whole file */
    bool save;
    unsigned attempts;
} config;

static bool parse_u64(const char *s, int base, uint64_t *v)
{
    char *end = NULL;
    if (*s == '\0') {
        return false;
    }
    *v = strtoull(s, &end, base);
    return *end == '\0';
}

/* Returns false (after saying why) if the file is missing or wrong. */
static bool read_config(config *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->attempts = DEFAULT_ATTEMPTS;
    FILE *f = fopen(CONFIG, "rb");
    if (f == NULL) {
        out("No %s.\n\nCreate it on the PC and copy it over FTP, e.g.:\n"
            "  url=http://192.168.1.10:8080/xcat-test-1073741824.bin\n"
            "  size=1073741824\n  crc32=<from make_test_file.py>\n",
            CONFIG);
        return false;
    }
    char line[XC_URL_MAX + 64];
    bool ok = true;
    while (ok && fgets(line, sizeof line, f) != NULL) {
        size_t len = strlen(line);
        while (len > 0 &&
               (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' ')) {
            line[--len] = '\0';
        }
        if (len == 0 || line[0] == '#') {
            continue;
        }
        char *eq = strchr(line, '=');
        if (eq == NULL) {
            out("%s: bad line: %s\n", CONFIG, line);
            ok = false;
            break;
        }
        *eq = '\0';
        const char *key = line;
        const char *val = eq + 1;
        uint64_t v = 0;
        if (strcmp(key, "url") == 0 && strlen(val) <= XC_URL_MAX) {
            memcpy(cfg->url, val, strlen(val) + 1);
        } else if (strcmp(key, "size") == 0 && parse_u64(val, 10, &v)) {
            cfg->has_size = true;
            cfg->size = v;
        } else if (strcmp(key, "crc32") == 0 && parse_u64(val, 16, &v) && v <= 0xFFFFFFFFU) {
            cfg->has_crc = true;
            cfg->crc = (uint32_t)v;
        } else if (strcmp(key, "limit") == 0 && parse_u64(val, 10, &v) && v > 0) {
            cfg->limit = v;
        } else if (strcmp(key, "save") == 0) {
            cfg->save = strcmp(val, "yes") == 0;
        } else if (strcmp(key, "attempts") == 0 && parse_u64(val, 10, &v) && v > 0 && v <= 1000) {
            cfg->attempts = (unsigned)v;
        } else {
            out("%s: bad value for %s: %s\n", CONFIG, key, val);
            ok = false;
        }
    }
    fclose(f);
    if (ok && cfg->url[0] == '\0') {
        out("%s: no url= line\n", CONFIG);
        ok = false;
    }
    return ok;
}

/* --- download ------------------------------------------------------------------------- */

typedef struct run {
    const xc_clock *clock;
    uint64_t size;
    uint64_t done;
    uint32_t crc;
    uint64_t start_ms;
    uint64_t next_report_ms;
    uint64_t last_data_ms;
    uint64_t longest_gap_ms;
    unsigned retries;
    /* save=yes */
    HANDLE save;
    uint8_t *wbuf;
    size_t wlen;
    uint64_t write_ms;
} run;

static uint64_t now_ms(const run *r)
{
    return r->clock->now_ms(r->clock->ctx);
}

/* "12.3" from tenths, since PDCLib's printf has no floating point. */
static void tenths(char *buf, size_t size, uint64_t t)
{
    (void)snprintf(buf, size, "%llu.%llu", (unsigned long long)(t / 10U),
                   (unsigned long long)(t % 10U));
}

/* Decimal MB/s, in tenths (docs/testing.md records MB/s). */
static uint64_t mbps_tenths(uint64_t bytes, uint64_t ms)
{
    return ms > 0 ? bytes / ms / 100U : 0;
}

/* "2026-10-16 14:05:00" (UTC) from Unix seconds; days to a civil date as in H. Hinnant's
 * "chrono-Compatible Low-Level Date Algorithms". */
static void utc_date(char *buf, size_t size, int64_t t)
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

static bool flush_save(run *r)
{
    if (r->wlen == 0) {
        return true;
    }
    uint64_t t0 = now_ms(r);
    DWORD put = 0;
    bool ok = WriteFile(r->save, r->wbuf, (DWORD)r->wlen, &put, NULL) && put == r->wlen;
    r->write_ms += now_ms(r) - t0;
    r->wlen = 0;
    return ok;
}

static xc_err on_data(void *ctx, uint64_t offset, const uint8_t *data, size_t len)
{
    run *r = ctx;
    (void)offset; /* always r->done: bytes arrive in order, each once */
    r->crc = xc_crc32(r->crc, data, len);
    r->done += len;
    uint64_t now = now_ms(r);
    if (r->last_data_ms != 0 && now - r->last_data_ms > r->longest_gap_ms) {
        r->longest_gap_ms = now - r->last_data_ms;
    }
    r->last_data_ms = now;
    while (r->save != INVALID_HANDLE_VALUE && len > 0) {
        size_t n = WRITE_BUF_SIZE - r->wlen < len ? WRITE_BUF_SIZE - r->wlen : len;
        memcpy(r->wbuf + r->wlen, data, n);
        r->wlen += n;
        data += n;
        len -= n;
        if (r->wlen == WRITE_BUF_SIZE && !flush_save(r)) {
            return XC_ERR_IO;
        }
    }
    if (now >= r->next_report_ms) {
        r->next_report_ms = now + REPORT_EVERY_MS;
        char speed[24];
        tenths(speed, sizeof speed, mbps_tenths(r->done, now - r->start_ms));
        out("  %4llu s  %llu of %llu MiB  %s MB/s\n",
            (unsigned long long)((now - r->start_ms) / 1000U), (unsigned long long)(r->done >> 20U),
            (unsigned long long)(r->size >> 20U), speed);
    }
    return XC_OK;
}

static bool on_retry(void *ctx, const xc_http_retry *info)
{
    run *r = ctx;
    r->retries++;
    char delay[24];
    tenths(delay, sizeof delay, info->delay_ms / 100U);
    uint64_t t = (now_ms(r) - r->start_ms) / 1000U;
    if (info->status != 0) {
        out("  %4llu s  HTTP %d, retrying in %s s (failure %u of %u)\n", (unsigned long long)t,
            info->status, delay, info->failures, info->max_attempts);
    } else {
        out("  %4llu s  %s, retrying in %s s (failure %u of %u)\n", (unsigned long long)t,
            xc_err_str(info->err), delay, info->failures, info->max_attempts);
    }
    return true;
}

/* --- TLS (M5) ------------------------------------------------------------------------- */

static bool is_https(const char *url)
{
    return strncmp(url, "https://", 8) == 0 || strncmp(url, "HTTPS://", 8) == 0;
}

static char *read_ca_file(size_t *len)
{
    HANDLE h = CreateFileA(CA_FILE, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return NULL;
    }
    char *data = malloc(XC_TLS_MAX_ANCHOR_FILE + 1);
    DWORD got = 0;
    bool ok = data != NULL && ReadFile(h, data, XC_TLS_MAX_ANCHOR_FILE + 1, &got, NULL);
    CloseHandle(h);
    if (!ok) {
        free(data);
        return NULL;
    }
    *len = got; /* one byte more than allowed is caught by the parser's limit */
    return data;
}

/* Sets up TLS over `tcp` and reports each step; false (after saying why) if it can't. */
static bool tls_setup(const xc_net *tcp, const xc_clock *clock, xc_net *tls_net, xc_tls **tls)
{
    static xc_entropy_pool *pool;
    static xc_entropy entropy;
    static xc_wallclock wallclock;
    static xc_tls_anchors *anchors;

    xc_tls_speed sp;
    out("Cipher speed on this CPU (%u MiB in memory, no network)...\n",
        (unsigned)(CIPHER_TEST_BYTES >> 20U));
    if (xc_tls_measure_ciphers(clock, CIPHER_TEST_BYTES, &sp) == XC_OK) {
        char a[24];
        char b[24];
        tenths(a, sizeof a, sp.chacha20_poly1305_kbs / 100U);
        tenths(b, sizeof b, sp.aes128_gcm_kbs / 100U);
        out("  ChaCha20-Poly1305 %s MB/s, AES-128-GCM %s MB/s\n", a, b);
    }

    char date[40];
    char floor_date[40];
    utc_date(date, sizeof date, xc_nx_console_time());
    utc_date(floor_date, sizeof floor_date, xc_nx_build_time());
    out("Console clock %s (this build: %s)\n", date, floor_date);
    xc_nx_wallclock(&wallclock);
    int64_t now = 0;
    if (wallclock.now(wallclock.ctx, &now) != XC_OK) {
        out("The console's clock is earlier than this XBE's build, so it was reset. Set the\n"
            "date and time in the dashboard's settings, then run the test again.\n");
        return false;
    }

    xc_nx_entropy_stats st;
    xc_err err = xc_entropy_pool_create(&pool);
    if (err == XC_OK) {
        err = xc_nx_entropy_gather(pool, &st);
        out("Randomness: %u timer samples, %u bits credited, %u of 256 low bytes seen, "
            "%llu-%llu cycles; seed file %s, %s\n",
            st.samples, st.bits, st.distinct_low_bytes, (unsigned long long)st.min_cycles,
            (unsigned long long)st.max_cycles, st.seed_read ? "read" : "none yet",
            st.seed_written ? "renewed" : "NOT written");
    }
    if (err != XC_OK) {
        out("Not enough randomness for TLS: %s\n", xc_err_str(err));
        return false;
    }
    xc_entropy_pool_source(pool, &entropy);

    size_t len = 0;
    char *pem = read_ca_file(&len);
    size_t bad = 0;
    if (pem != NULL) {
        err = xc_tls_anchors_parse(pem, len, &anchors, &bad);
        free(pem);
        if (err != XC_OK) {
            out("%s can't be used (%s, certificate %u). Fix or delete it.\n", CA_FILE,
                xc_err_str(err), (unsigned)bad);
            return false;
        }
        out("Trusting the %u certificates in %s\n", (unsigned)xc_tls_anchors_count(anchors),
            CA_FILE);
    } else {
        err = xc_tls_anchors_builtin(&anchors);
        if (err != XC_OK) {
            out("Built-in CA bundle: %s\n", xc_err_str(err));
            return false;
        }
        out("Trusting the %u built-in roots (Mozilla, %s)\n",
            (unsigned)xc_tls_anchors_count(anchors), xc_tls_builtin_date());
    }
    xc_tls_config tc = {tcp, anchors, &entropy, &wallclock};
    err = xc_tls_create(&tc, tls, tls_net);
    if (err != XC_OK) {
        out("TLS: %s\n", xc_err_str(err));
        return false;
    }
    return true;
}

static void tls_report(const xc_tls *tls)
{
    xc_tls_info info;
    xc_tls_last_info(tls, &info);
    if (info.suite_name != NULL) {
        out("TLS %u.%u, %s (suite 0x%04x)\n", (info.version >> 8) - 2U, (info.version & 0xFFU) - 1U,
            info.suite_name, (unsigned)info.cipher_suite);
    }
}

int main(void)
{
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    debugPrint("xiso-cartographer %s - network test (M5)\n\n", xc_version_string());
    if (!xc_nx_mount_e() || !xc_nx_mkdirs("E:\\xcat")) {
        debugPrint("Can't use the E: drive\n");
        halt();
    }
    /* Each run is appended, so a run after a reboot doesn't erase the one before it. */
    log_file = CreateFileA(LOG, GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (log_file != INVALID_HANDLE_VALUE) {
        DWORD end = SetFilePointer(log_file, 0, NULL, FILE_END);
        if (end == INVALID_SET_FILE_POINTER || end > LOG_MAX_BYTES) {
            SetFilePointer(log_file, 0, NULL, FILE_BEGIN); /* too big or unreadable: start over */
            SetEndOfFile(log_file);
        } else if (end > 0) {
            DWORD put = 0;
            (void)WriteFile(log_file, "\n", 1, &put, NULL);
        }
    }
    out("==== xiso-cartographer %s network test ====\n", xc_version_string());
    static config cfg;
    if (!read_config(&cfg)) {
        halt();
    }

    char ip[32] = "";
    out("Starting the network (dashboard settings)...\n");
    if (xc_nx_net_startup(ip, sizeof ip) != XC_OK) {
        out("No network: check the cable and the dashboard's network settings (DHCP gave no "
            "address).\n");
        halt();
    }
    out("Console IP %s\nURL %s\n", ip, cfg.url);

    xc_net net;
    xc_clock clock;
    xc_nx_net(&net);
    xc_nx_clock(&clock);
    static xc_net tls_net;
    xc_tls *tls = NULL;
    if (is_https(cfg.url) && !tls_setup(&net, &clock, &tls_net, &tls)) {
        halt();
    }
    uint8_t *rbuf = malloc(RECV_BUF_SIZE);
    static run r;
    memset(&r, 0, sizeof r);
    r.clock = &clock;
    r.save = INVALID_HANDLE_VALUE;
    static xc_http_resource res; /* large (three URLs): keep it off the stack */
    xc_err err =
        rbuf != NULL ? xc_http_resource_init(&res, cfg.url, strlen(cfg.url)) : XC_ERR_NOMEM;
    xc_http_config hc;
    memset(&hc, 0, sizeof hc);
    hc.net = &net;
    hc.tls = tls != NULL ? &tls_net : NULL;
    hc.clock = &clock;
    hc.buf = rbuf;
    hc.buf_size = RECV_BUF_SIZE;
    hc.max_attempts = cfg.attempts;
    hc.seed = (uint32_t)clock.now_ms(clock.ctx);
    hc.on_retry = on_retry;
    hc.retry_ctx = &r;
    static xc_http_client client;
    memset(&client, 0, sizeof client);
    if (err == XC_OK) {
        err = xc_http_client_init(&client, &hc);
    }
    r.start_ms = now_ms(&r);
    if (err == XC_OK) {
        err = xc_http_probe(&client, &res);
    }
    uint64_t total = 0;
    if (err == XC_OK) {
        out("Size %llu bytes (%llu MiB); connected in %llu ms\n", (unsigned long long)res.size,
            (unsigned long long)(res.size >> 20U), (unsigned long long)(now_ms(&r) - r.start_ms));
        if (tls != NULL) {
            tls_report(tls);
        }
        if (cfg.has_size && cfg.size != res.size) {
            out("  but net-test.txt says %llu: wrong file?\n", (unsigned long long)cfg.size);
        }
        total = cfg.limit != 0 && cfg.limit < res.size ? cfg.limit : res.size;
        r.size = total;
        if (total < res.size) {
            out("Downloading only the first %llu bytes (limit=)\n", (unsigned long long)total);
        }
    }
    if (err == XC_OK && cfg.save) {
        r.wbuf = malloc(WRITE_BUF_SIZE);
        r.save = r.wbuf != NULL ? CreateFileA(SAVE, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                              FILE_ATTRIBUTE_NORMAL, NULL)
                                : INVALID_HANDLE_VALUE;
        if (r.save == INVALID_HANDLE_VALUE) {
            err = XC_ERR_IO;
        } else {
            out("Also writing it to %s\n", SAVE);
        }
    }
    if (err == XC_OK) {
        out("Downloading. Pull the network cable any time: it should resume where it stopped.\n"
            "(A power cut or reboot starts the test over.)\n");
        r.start_ms = now_ms(&r);
        r.next_report_ms = r.start_ms + REPORT_EVERY_MS;
        err = xc_http_read(&client, &res, 0, total, on_data, &r);
        if (err == XC_OK && !flush_save(&r)) {
            err = XC_ERR_IO;
        }
    }
    uint64_t ms = now_ms(&r) - r.start_ms;
    xc_http_client_close(&client);
    if (r.save != INVALID_HANDLE_VALUE) {
        CloseHandle(r.save);
    }

    out("\n");
    if (err != XC_OK) {
        out("FAILED after %llu bytes: %s", (unsigned long long)r.done, xc_err_str(err));
        if (client.last_status != 0) {
            out(" (HTTP %d)", client.last_status);
        }
        if (tls != NULL && xc_tls_last_error(tls) != 0) {
            out(" (TLS error %d)", xc_tls_last_error(tls));
        }
        out("\n");
        halt();
    }
    char speed[24];
    char gap[24];
    tenths(speed, sizeof speed, mbps_tenths(r.done, ms));
    tenths(gap, sizeof gap, r.longest_gap_ms / 100U);
    out("Downloaded %llu bytes in %llu s: %s MB/s average\n", (unsigned long long)r.done,
        (unsigned long long)(ms / 1000U), speed);
    if (tls != NULL) {
        tls_report(tls); /* after redirects: the server the data came from */
    }
    out("Retries %u, longest pause in the data %s s\n", r.retries, gap);
    xc_nx_tcp_counters tc;
    xc_nx_tcp_counters_get(&tc);
    out("TCP: window %lu KB; frames in %lu, dropped %lu; segments in %lu, out %lu, bad %lu, "
        "dropped %lu; out of memory %lu\n",
        (unsigned long)(tc.window_bytes / 1024U), (unsigned long)tc.frames_in,
        (unsigned long)tc.frames_dropped, (unsigned long)tc.segments_in,
        (unsigned long)tc.segments_out, (unsigned long)tc.segments_bad,
        (unsigned long)tc.segments_dropped, (unsigned long)tc.out_of_memory);
    if (cfg.save) {
        char wspeed[24];
        tenths(wspeed, sizeof wspeed, mbps_tenths(r.done, r.write_ms));
        out("Disk writes took %llu s (%s MB/s while writing)\n",
            (unsigned long long)(r.write_ms / 1000U), wspeed);
    }
    out("CRC-32 %08lx", (unsigned long)r.crc);
    if (cfg.has_crc && total < res.size) {
        out(" (of the first %llu bytes; crc32= is for the whole file)\n",
            (unsigned long long)total);
    } else if (cfg.has_crc && r.crc == cfg.crc) {
        out(": matches, PASS\n");
    } else if (cfg.has_crc) {
        out(": expected %08lx, FAIL\n", (unsigned long)cfg.crc);
    } else {
        out(" (no crc32= in net-test.txt to compare with)\n");
    }
    halt();
}
