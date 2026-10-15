/*
 * Network test XBE (M3): downloads one file over plain HTTP with the core client and
 * reports MB/s, every retry (e.g. while the cable is pulled) and the CRC-32.
 *
 * Reads E:\xcat\net-test.txt, one "key=value" per line:
 *
 *   url=http://192.168.1.10:8080/xcat-test-1073741824.bin   (required)
 *   size=1073741824     expected size in bytes (optional)
 *   crc32=0123abcd      expected CRC-32 (optional; tools/testserver/make_test_file.py prints both)
 *   save=yes            also write the file to E:\xcat\net-test.bin (network + disk speed)
 *   attempts=40         consecutive failures before giving up (default 40, about 30 minutes)
 *
 * The file is served from a PC with tools/testserver/rangeserver.py. Results go on screen
 * and are appended to E:\xcat\net-test.log. Resume works within a run (lost connection);
 * a power cut starts over, since nothing is journaled to disk until M4. See docs/testing.md
 * ("M3 network test").
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
#include "xcat/version.h"

#define CONFIG "E:\\xcat\\net-test.txt"
#define LOG "E:\\xcat\\net-test.log"
#define SAVE "E:\\xcat\\net-test.bin"
#define RECV_BUF_SIZE ((size_t)256 * 1024)
#define WRITE_BUF_SIZE ((size_t)1024 * 1024) /* large writes: FATX is slow with small ones */
#define DEFAULT_ATTEMPTS 40U
#define REPORT_EVERY_MS 5000U
#define LOG_MAX_BYTES (1024U * 1024U) /* the log is appended to; past this it starts over */

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

int main(void)
{
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    debugPrint("xiso-cartographer %s - network test (M3)\n\n", xc_version_string());
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
    if (err == XC_OK) {
        r.size = res.size;
        out("Size %llu bytes (%llu MiB)\n", (unsigned long long)res.size,
            (unsigned long long)(res.size >> 20U));
        if (cfg.has_size && cfg.size != res.size) {
            out("  but net-test.txt says %llu: wrong file?\n", (unsigned long long)cfg.size);
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
        /* Progress lives in memory only: resuming after a power cut or reboot is M4's journal. */
        out("Downloading. Pull the network cable any time: it should resume where it stopped.\n"
            "(A power cut or reboot starts the test over; resuming across those comes in M4.)\n");
        r.start_ms = now_ms(&r);
        r.next_report_ms = r.start_ms + REPORT_EVERY_MS;
        err = xc_http_read(&client, &res, 0, res.size, on_data, &r);
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
        out("\n");
        halt();
    }
    char speed[24];
    char gap[24];
    tenths(speed, sizeof speed, mbps_tenths(r.done, ms));
    tenths(gap, sizeof gap, r.longest_gap_ms / 100U);
    out("Downloaded %llu bytes in %llu s: %s MB/s average\n", (unsigned long long)r.done,
        (unsigned long long)(ms / 1000U), speed);
    out("Retries %u, longest pause in the data %s s\n", r.retries, gap);
    if (cfg.save) {
        char wspeed[24];
        tenths(wspeed, sizeof wspeed, mbps_tenths(r.done, r.write_ms));
        out("Disk writes took %llu s (%s MB/s while writing)\n",
            (unsigned long long)(r.write_ms / 1000U), wspeed);
    }
    out("CRC-32 %08lx", (unsigned long)r.crc);
    if (cfg.has_crc && r.crc == cfg.crc) {
        out(": matches, PASS\n");
    } else if (cfg.has_crc) {
        out(": expected %08lx, FAIL\n", (unsigned long)cfg.crc);
    } else {
        out(" (no crc32= in net-test.txt to compare with)\n");
    }
    halt();
}
