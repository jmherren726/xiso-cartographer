/*
 * xiso-cartographer XBE, milestone M5b: install games over the network (HTTP or HTTPS),
 * resumably, from images or from zips holding one (ADR 0009).
 *
 * What it installs, in this order:
 *   1. every entry of the map E:\xcat\map.txt (docs/formats/map.md), downloaded over HTTP
 *      with Range and installed while it streams in (ADR 0004); an entry ending in .zip
 *      installs the zip's image (docs/formats/zip.md, stream-install.md);
 *   2. every image or zip in E:\xcat\isos\ (the M2 local install);
 *   3. if there is neither, the bundled synthetic test image (D:\samples\*.iso).
 * Each one goes through xc_install (xcat/install.h): FATX check, free space, a journal so a
 * pulled cable or a power cut resumes instead of starting over, a read-back check of every
 * file, and the .xcat-installed marker. Logins come from E:\Apps\xcat\credentials
 * (docs/formats/credentials.md). Everything shown is also appended to E:\xcat\install.log.
 * The real UI replaces this in M8.
 */
#include <SDL.h>
#include <hal/debug.h>
#include <hal/video.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include "xcat/credentials.h"
#include "xcat/http_client.h"
#include "xcat/install.h"
#include "xcat/map.h"
#include "xcat/nxdk.h"
#include "xcat/version.h"
#include "xcat/zip.h"

#define XCAT_DIR "E:\\xcat"
#define MAP_FILE XCAT_DIR "\\map.txt"
#define ISO_DIR XCAT_DIR "\\isos"
#define LOG_FILE XCAT_DIR "\\install.log"
#define CRED_FILE "E:\\Apps\\xcat\\credentials"
#define SAMPLE_DIR "D:\\samples"
#define GAMES_DIR "E:\\Games"
#define IO_BUF_SIZE ((size_t)1024 * 1024)
#define RECV_BUF_SIZE ((size_t)256 * 1024)
#define CLUSTER 16384U /* FATX cluster size on a retail E: partition; for the space estimate */
#define ATTEMPTS 40U   /* consecutive failures before giving up: about half an hour */
#define MAX_LOCAL 16
#define LOG_MAX_BYTES (1024U * 1024U)
#define REPORT_EVERY_MS 5000U

static uint8_t *io_buf;
static uint8_t *recv_buf;
static HANDLE log_file = INVALID_HANDLE_VALUE;

/* --- screen and log ---------------------------------------------------------------- */

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
        IO_STATUS_BLOCK io; /* flushed: the run usually ends at the power switch */
        (void)NtFlushBuffersFile(log_file, &io);
    }
}

static void open_log(void)
{
    log_file =
        CreateFileA(LOG_FILE, GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (log_file == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD end = SetFilePointer(log_file, 0, NULL, FILE_END);
    if (end == INVALID_SET_FILE_POINTER || end > LOG_MAX_BYTES) {
        SetFilePointer(log_file, 0, NULL, FILE_BEGIN);
        SetEndOfFile(log_file);
    } else if (end > 0) {
        DWORD put = 0;
        (void)WriteFile(log_file, "\n", 1, &put, NULL);
    }
}

/* --- controllers: LT + RT + Back + Black returns to the dashboard ------------------- */

#define MAX_PADS 4
#define PAD_POLL_MS 100U
#define TRIGGER_DOWN 16384 /* of 32767 */

static bool pads_ok;
static SDL_GameController *pads[MAX_PADS];
static DWORD next_pad_poll;
static bool exit_requested;

static void pads_init(void)
{
    pads_ok = SDL_Init(SDL_INIT_GAMECONTROLLER) == 0;
}

/* True once LT + RT + Back + Black (the rocky5 shortcut) is held on any controller. Cheap to
   call often: it looks at the controllers at most every 100 ms. */
static bool exit_combo(void)
{
    if (!pads_ok || exit_requested || GetTickCount() - next_pad_poll > 0x80000000U) {
        return exit_requested;
    }
    next_pad_poll = GetTickCount() + PAD_POLL_MS;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_CONTROLLERDEVICEADDED) {
            for (int i = 0; i < MAX_PADS; i++) {
                if (pads[i] == NULL) {
                    pads[i] = SDL_GameControllerOpen(e.cdevice.which);
                    break;
                }
            }
        } else if (e.type == SDL_CONTROLLERDEVICEREMOVED) {
            SDL_GameController *gone = SDL_GameControllerFromInstanceID(e.cdevice.which);
            for (int i = 0; i < MAX_PADS; i++) {
                if (pads[i] != NULL && pads[i] == gone) {
                    SDL_GameControllerClose(pads[i]);
                    pads[i] = NULL;
                }
            }
        }
    }
    SDL_GameControllerUpdate();
    for (int i = 0; i < MAX_PADS; i++) {
        SDL_GameController *c = pads[i];
        if (c != NULL &&
            SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > TRIGGER_DOWN &&
            SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > TRIGGER_DOWN &&
            SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_BACK) &&
            SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) { /* Black */
            exit_requested = true;
        }
    }
    return exit_requested;
}

static void close_log(void)
{
    if (log_file != INVALID_HANDLE_VALUE) {
        CloseHandle(log_file);
        log_file = INVALID_HANDLE_VALUE;
    }
}

/* Back to the dashboard. Everything written so far is on the disk already (journal and
   checkpoints are flushed as they go), so an unfinished install resumes next time. */
static void to_dashboard(void)
{
    out("\nReturning to the dashboard...\n");
    close_log();
    HalReturnToFirmware(HalQuickRebootRoutine);
}

static void halt(void)
{
    close_log();
    for (;;) {
        if (exit_combo()) {
            to_dashboard();
        }
        Sleep(PAD_POLL_MS);
    }
}

/* "12.3" from tenths: PDCLib's printf has no floating point. */
static void tenths(char *buf, size_t size, uint64_t t)
{
    (void)snprintf(buf, size, "%llu.%llu", (unsigned long long)(t / 10U),
                   (unsigned long long)(t % 10U));
}

/* --- network, on first use ------------------------------------------------------------ */

static bool net_tried;
static bool net_up;
static xc_http_config http_cfg;
static bool https_tried;                      /* TLS was set up, or tried (https_ready) */
static xc_err https_err = XC_ERR_UNSUPPORTED; /* why there is no TLS, if there isn't */
static xc_tls *tls;
static xc_net tls_net;
static xc_net net;
static xc_clock clk;
static xc_credentials creds;
static xc_http_auth auth;
static bool have_creds;
static xc_http_client client;
static uint64_t run_start_ms;

static uint64_t now_ms(void)
{
    return clk.now_ms(clk.ctx);
}

static bool on_retry(void *ctx, const xc_http_retry *r)
{
    (void)ctx;
    xc_nx_net_stats st;
    xc_nx_net_last(&st);
    char delay[24];
    tenths(delay, sizeof delay, r->delay_ms / 100U);
    unsigned long long t = (unsigned long long)((now_ms() - run_start_ms) / 1000U);
    if (r->status != 0) {
        out("  %4llu s  HTTP %d, retrying in %s s (failure %u of %u)\n", t, r->status, delay,
            r->failures, r->max_attempts);
    } else {
        out("  %4llu s  %s, retrying in %s s (failure %u of %u)\n", t, xc_err_str(r->err), delay,
            r->failures, r->max_attempts);
    }
    /* Which step was slow (diagnosis for the first-request timeout seen in M3). */
    out("           last connect: lookup %lu ms, handshake %lu ms, %s\n", (unsigned long)st.dns_ms,
        (unsigned long)st.connect_ms, xc_err_str(st.result));
    return !exit_combo(); /* the shortcut also works while waiting out an outage */
}

static void load_credentials(void)
{
    if (!xc_nx_exists(CRED_FILE)) {
        /* credentials.md §2: not an error, but say so; a .txt name is an easy mistake */
        out("No credentials: %s doesn't exist%s\n", CRED_FILE,
            xc_nx_exists(CRED_FILE ".txt") ? " (there is a credentials.txt: rename it to "
                                             "credentials, without .txt)"
                                           : "");
        return;
    }
    xc_nx_file f = {NULL};
    xc_source src;
    char *data = NULL;
    xc_err err = xc_nx_file_open(&src, &f, CRED_FILE);
    if (err == XC_OK && src.size > XC_CRED_MAX_FILE) {
        err = XC_ERR_LIMIT;
    }
    if (err == XC_OK) {
        data = malloc((size_t)src.size + 1);
        err = data != NULL ? xc_source_read(&src, 0, data, (size_t)src.size) : XC_ERR_NOMEM;
    }
    xc_cred_error cerr;
    memset(&cerr, 0, sizeof cerr);
    if (err == XC_OK) {
        err = xc_cred_parse(data, (size_t)src.size, &creds, &cerr);
    }
    free(data);
    xc_nx_file_close(&f);
    if (err == XC_OK) {
        have_creds = true;
        out("Credentials: %u section(s) from %s\n", (unsigned)creds.section_count, CRED_FILE);
    } else if (err == XC_ERR_CREDENTIALS) {
        out("%s line %u: %s; installing without credentials\n", CRED_FILE, (unsigned)cerr.line,
            cerr.message);
    } else {
        out("%s: %s; installing without credentials\n", CRED_FILE, xc_err_str(err));
    }
}

/* xc_nx_https_start reports each step through this. */
static void say(const char *line)
{
    out("%s", line);
}

static bool is_https(const char *url)
{
    return strncmp(url, "https://", 8) == 0 || strncmp(url, "HTTPS://", 8) == 0;
}

static bool network(void)
{
    if (net_tried) {
        return net_up;
    }
    net_tried = true;
    char ip[32] = "";
    out("Starting the network (dashboard settings)...\n");
    if (xc_nx_net_startup(ip, sizeof ip) != XC_OK) {
        out("No network: check the cable and the dashboard's network settings.\n");
        return false;
    }
    out("Console IP %s\n", ip);
    xc_nx_net(&net);
    memset(&http_cfg, 0, sizeof http_cfg);
    http_cfg.net = &net;
    http_cfg.clock = &clk;
    http_cfg.buf = recv_buf;
    http_cfg.buf_size = RECV_BUF_SIZE;
    http_cfg.max_attempts = ATTEMPTS;
    http_cfg.seed = (uint32_t)now_ms();
    http_cfg.on_retry = on_retry;
    if (have_creds) {
        xc_cred_http_auth(&auth, &creds);
        http_cfg.auth = &auth;
    }
    net_up = xc_http_client_init(&client, &http_cfg) == XC_OK;
    return net_up;
}

/* The first https URL sets up TLS (ADR 0012: randomness, NTP, trust) and gives it to the
   HTTP client. False, after saying why once, if HTTPS can't be used. */
static bool https_ready(void)
{
    if (https_tried) {
        return https_err == XC_OK;
    }
    https_tried = true;
    char ntp_buf[256];
    xc_nx_https_opts o = {&net, &clk, xc_nx_ntp_setting(ntp_buf, sizeof ntp_buf), say};
    https_err = xc_nx_https_start(&o, &tls_net, &tls);
    if (https_err == XC_OK) {
        http_cfg.tls = &tls_net;
        xc_http_client_close(&client);
        https_err = xc_http_client_init(&client, &http_cfg);
    }
    if (https_err != XC_OK) {
        out("HTTPS isn't available: %s. http:// entries still install.\n", xc_err_str(https_err));
    }
    return https_err == XC_OK;
}

/* --- the map ------------------------------------------------------------------------------ */

static xc_err read_local(const char *path, size_t max_len, char **data, size_t *len)
{
    xc_nx_file f = {NULL};
    xc_source src;
    xc_err err = xc_nx_file_open(&src, &f, path);
    if (err == XC_OK && src.size > max_len) {
        err = XC_ERR_LIMIT;
    }
    char *buf = NULL;
    if (err == XC_OK) {
        buf = malloc((size_t)src.size + 1);
        err = buf != NULL ? xc_source_read(&src, 0, buf, (size_t)src.size) : XC_ERR_NOMEM;
    }
    xc_nx_file_close(&f);
    if (err != XC_OK) {
        free(buf);
        return err;
    }
    *data = buf;
    *len = (size_t)src.size;
    return XC_OK;
}

typedef struct mem_ctx {
    char *buf;
    size_t len;
    size_t cap;
} mem_ctx;

/* xc_http_data_fn into a growing buffer. A body that starts over at 0 (xc_http_get after a
 * retry) replaces what came before. */
static xc_err to_mem(void *ctx, uint64_t offset, const uint8_t *data, size_t len)
{
    mem_ctx *m = ctx;
    m->len = (size_t)offset;
    if (m->cap - m->len <= len) {
        size_t cap = m->cap == 0 ? (size_t)64 * 1024 : m->cap;
        while (cap - m->len <= len) {
            cap *= 2;
        }
        char *grown = realloc(m->buf, cap);
        if (grown == NULL) {
            return XC_ERR_NOMEM;
        }
        m->buf = grown;
        m->cap = cap;
    }
    memcpy(m->buf + m->len, data, len);
    m->len += len;
    return XC_OK;
}

static xc_http_resource res; /* large: one at a time */

/* xc_map_fetch_fn: includes from the HDD or over HTTP. */
static xc_err map_fetch(void *ctx, const char *location, bool is_url, size_t max_len, char **data,
                        size_t *len)
{
    (void)ctx;
    if (!is_url) {
        return read_local(location, max_len, data, len);
    }
    if (!network()) {
        return XC_ERR_NET_CONNECT;
    }
    if (is_https(location) && !https_ready()) {
        return https_err;
    }
    xc_err err = xc_http_resource_init(&res, location, strlen(location));
    mem_ctx m = {NULL, 0, 0};
    if (err == XC_OK) {
        err = xc_http_get(&client, &res, max_len, to_mem, &m);
    }
    if (err == XC_OK && m.buf == NULL) {
        m.buf = malloc(1); /* an empty body */
        err = m.buf != NULL ? XC_OK : XC_ERR_NOMEM;
    }
    if (err != XC_OK) {
        free(m.buf);
        return err;
    }
    *data = m.buf;
    *len = m.len;
    return XC_OK;
}

static bool load_map(xc_map *map)
{
    memset(map, 0, sizeof *map);
    if (!xc_nx_exists(MAP_FILE)) {
        return false;
    }
    char *data = NULL;
    size_t len = 0;
    xc_err err = read_local(MAP_FILE, XC_MAP_MAX_FILE, &data, &len);
    if (err != XC_OK) {
        out("%s: %s\n", MAP_FILE, err == XC_ERR_LIMIT ? "larger than 1 MiB" : xc_err_str(err));
        return false;
    }
    xc_map_loader loader = {map_fetch, NULL};
    err = xc_map_load(data, len, MAP_FILE, &loader, map);
    free(data);
    for (size_t i = 0; i < map->problem_count; i++) {
        const xc_map_problem *p = &map->problems[i];
        out("%s:%u: %s: %s\n", map->sources[p->source], (unsigned)p->line,
            p->severity == XC_MAP_ERROR ? "error" : "warning", p->message);
    }
    if (map->problems_dropped > 0) {
        out("... and %u more map problems\n", (unsigned)map->problems_dropped);
    }
    out("Map: %u entr%s from %u file(s)\n", (unsigned)map->entry_count,
        map->entry_count == 1 ? "y" : "ies", (unsigned)map->source_count);
    return err == XC_OK;
}

/* --- installing --------------------------------------------------------------------------- */

static bool ends_with_ci(const char *s, size_t len, const char *suffix)
{
    size_t n = strlen(suffix);
    if (len < n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char a = s[len - n + i];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (a != suffix[i]) {
            return false;
        }
    }
    return true;
}

static int hexval(char c)
{
    return c >= '0' && c <= '9'   ? c - '0'
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
           : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                  : -1;
}

/* "dir/Halo%20(USA).xiso.iso?x" -> "Halo (USA)"; likewise "Halo (USA).zip" */
static void title_from(const char *name, char *out_title, size_t size)
{
    const char *end = name + strcspn(name, "?#");
    const char *p = end;
    while (p > name && p[-1] != '/' && p[-1] != '\\') {
        p--;
    }
    size_t n = 0;
    while (p < end && n + 1 < size) {
        int hi = p[0] == '%' && end - p >= 3 ? hexval(p[1]) : -1;
        int lo = hi >= 0 ? hexval(p[2]) : -1;
        out_title[n++] = lo >= 0 ? (char)(hi * 16 + lo) : *p;
        p += lo >= 0 ? 3 : 1;
    }
    if (ends_with_ci(out_title, n, ".zip")) {
        n -= 4;
    }
    if (ends_with_ci(out_title, n, ".iso")) {
        n -= 4;
    }
    if (ends_with_ci(out_title, n, ".xiso")) {
        n -= 5;
    }
    out_title[n] = '\0';
}

typedef struct ui {
    int phase;
    uint64_t next_ms;
    uint64_t phase_ms;    /* when the current phase began */
    uint64_t phase_bytes; /* bytes_done when it began: a resumed install starts above 0 */
    uint64_t write_ms;    /* how long writing took, once it's over */
    uint64_t write_bytes; /* bytes written in this run */
    uint64_t verify_ms;
    uint64_t verify_bytes;
} ui;

/* Decimal MB/s in tenths. */
static uint64_t mbps_tenths(uint64_t bytes, uint64_t ms)
{
    return ms > 0 ? bytes / ms / 100U : 0;
}

static void end_phase(ui *u, uint64_t now, uint64_t bytes_done)
{
    if (u->phase == XC_INSTALL_WRITING) {
        u->write_ms = now - u->phase_ms;
        u->write_bytes = bytes_done - u->phase_bytes;
    } else if (u->phase == XC_INSTALL_VERIFYING) {
        u->verify_ms = now - u->phase_ms;
        u->verify_bytes = bytes_done - u->phase_bytes;
    }
}

static uint64_t last_bytes_done;

static bool on_progress(void *ctx, const xc_install_progress *p)
{
    ui *u = ctx;
    uint64_t now = now_ms();
    if ((int)p->phase != u->phase) {
        end_phase(u, now, last_bytes_done);
        u->phase = (int)p->phase;
        u->next_ms = 0;
        u->phase_ms = now;
        u->phase_bytes = p->bytes_done;
        out("  %s\n", p->phase == XC_INSTALL_WRITING     ? "writing files"
                      : p->phase == XC_INSTALL_VERIFYING ? "verifying (reading every file back)"
                                                         : "reading the directory tables");
    }
    last_bytes_done = p->bytes_done;
    if (now >= u->next_ms && p->bytes_total > 0) {
        u->next_ms = now + REPORT_EVERY_MS;
        char speed[24] = "-";
        if (now > u->phase_ms + 1000U) {
            /* this run's bytes only: what a resume found on the disk isn't speed */
            tenths(speed, sizeof speed,
                   mbps_tenths(p->bytes_done - u->phase_bytes, now - u->phase_ms));
        }
        /* The file being worked on (1-based), not the number finished: one big file would
           otherwise read "0 of N" for most of an install. */
        size_t current = p->files_done < p->file_count ? p->files_done + 1 : p->file_count;
        out("  %4llu s  %llu of %llu MiB, file %u of %u, %s MB/s\n",
            (unsigned long long)((now - run_start_ms) / 1000U),
            (unsigned long long)(p->bytes_done >> 20U), (unsigned long long)(p->bytes_total >> 20U),
            (unsigned)current, (unsigned)p->file_count, speed);
    }
    return !exit_combo();
}

static const char *problem_text(const xc_fatx_problem *p)
{
    switch (p->kind) {
    case XC_FATX_PROBLEM_NAME:
        return p->name == XC_FATX_NAME_TOO_LONG ? "name over 42 characters" : "invalid name";
    case XC_FATX_PROBLEM_PATH_TOO_LONG:
        return "path over 250 characters";
    case XC_FATX_PROBLEM_TOO_MANY_ENTRIES:
        return "over 4096 entries in one folder";
    case XC_FATX_PROBLEM_FILE_TOO_BIG:
        return "file over 4 GiB - 16 KiB (FATX can't grow a file further)";
    }
    return "problem";
}

static xc_install_result result;

/* journal.md §7: the whole-image hashes (compared from M6, when archive.org gives them). */
static void log_image_hash(const xc_install_result *r)
{
    if (r->hash == XC_INSTALL_HASH_NONE) {
        out("  image not hashed: %s\n", r->hash_note != NULL ? r->hash_note : "?");
        return;
    }
    char md5[2 * XC_MD5_BYTES + 1];
    char sha1[2 * XC_SHA1_BYTES + 1];
    xc_hex_lower(r->md5, XC_MD5_BYTES, md5);
    xc_hex_lower(r->sha1, XC_SHA1_BYTES, sha1);
    out("  image MD5 %s\n  image SHA-1 %s%s\n", md5, sha1,
        r->hash_restarted ? " (hashed from the start again)" : "");
}

/* ADR 0009 amendment 1: how a zip install went, and what a Deflate one cost. */
static void log_zip(const xc_install_result *r)
{
    out("  from the zip: %s, %s\n", r->zip_member, r->zip_method == 0 ? "stored" : "Deflate");
    if (r->zip_method == 0) {
        return;
    }
    out("  %u pass%s, %llu MiB read again, %u file(s) from a %llu MiB spool\n", r->passes,
        r->passes == 1 ? "" : "es", (unsigned long long)(r->reread_bytes >> 20U),
        (unsigned)r->files_from_spool, (unsigned long long)(r->spool_peak >> 20U));
    if (r->zip_resumed_at > 0) {
        out("  continued %llu MiB into the image\n",
            (unsigned long long)(r->zip_resumed_at >> 20U));
    }
}

/* Installs one image; true if it ended installed and verified. */
static bool install(const xc_source *src, const char *name, const char *etag,
                    const char *last_modified)
{
    char title[128];
    title_from(name, title, sizeof title);
    out("\n%s\n", name);
    xc_install_io io;
    xc_nx_install_io(&io);
    ui u;
    memset(&u, 0, sizeof u);
    u.phase = -1;
    last_bytes_done = 0;
    xc_install_req req;
    memset(&req, 0, sizeof req);
    req.src = src;
    req.etag = etag;
    req.last_modified = last_modified;
    req.source_name = name;
    req.games_dir = GAMES_DIR;
    req.title = title;
    req.io = &io;
    req.buf = io_buf;
    req.buf_size = IO_BUF_SIZE;
    req.cluster_size = CLUSTER;
    req.progress = on_progress;
    req.progress_ctx = &u;
    req.zip = xc_zip_is_zip_name(name);
    xc_err err = xc_install(&req, &result);
    xc_nx_spool_close();
    end_phase(&u, now_ms(), last_bytes_done);
    if (result.other_folder_reason != NULL) {
        out("  (not the existing folder of that name: %s)\n", result.other_folder_reason);
    }
    if (err == XC_OK) {
        char wspeed[24];
        char vspeed[24];
        tenths(wspeed, sizeof wspeed, mbps_tenths(u.write_bytes, u.write_ms));
        tenths(vspeed, sizeof vspeed, mbps_tenths(u.verify_bytes, u.verify_ms));
        out("  OK: %u files in %s, verified%s\n", (unsigned)result.file_count, result.folder,
            result.resumed ? " (resumed)" : "");
        out("  %u written, %u kept from an earlier attempt\n", (unsigned)result.files_written,
            (unsigned)result.files_skipped);
        out("  wrote %llu MiB in %llu s (%s MB/s); read back %llu MiB in %llu s (%s MB/s)\n",
            (unsigned long long)(u.write_bytes >> 20U), (unsigned long long)(u.write_ms / 1000U),
            wspeed, (unsigned long long)(u.verify_bytes >> 20U),
            (unsigned long long)(u.verify_ms / 1000U), vspeed);
        if (result.files_continued > 0) {
            out("  %u file(s) continued from a checkpoint (%llu MiB were already there)\n",
                (unsigned)result.files_continued,
                (unsigned long long)(result.bytes_continued >> 20U));
        }
        if (req.zip) {
            log_zip(&result);
        }
        log_image_hash(&result);
        return true;
    }
    out("  NOT INSTALLED: %s\n", xc_err_str(err));
    if (err == XC_ERR_ZIP_UNSUPPORTED && result.zip_method != 0 && result.zip_method != 8) {
        const char *method = xc_zip_method_name(result.zip_method);
        out("   %s in the zip uses %s (method %u); only stored and Deflate can be read\n",
            result.zip_member[0] != '\0' ? result.zip_member : "the image",
            method != NULL ? method : "an unknown method", (unsigned)result.zip_method);
    }
    if (err == XC_ERR_IMAGE_HASH) {
        log_image_hash(&result);
    }
    if (result.current_file[0] != '\0') {
        out("   at %s\n", result.current_file);
    }
    const char *op = NULL;
    unsigned long code = 0;
    xc_nx_last_io_error(&op, &code);
    if (err == XC_ERR_IO && op != NULL) {
        out("   the disk %s failed (error 0x%08lX)\n", op, code);
    }
    if (err == XC_ERR_NOT_INSTALLABLE) {
        for (size_t i = 0; i < result.problem_count && i < XC_INSTALL_MAX_PROBLEMS; i++) {
            out("   %.48s: %s\n", result.problems[i].path, problem_text(&result.problems[i]));
        }
    } else if (err == XC_ERR_NO_SPACE) {
        out("   needs %llu MiB, %llu MiB free\n", (unsigned long long)(result.space_needed >> 20U),
            (unsigned long long)(result.space_free >> 20U));
    } else if (err == XC_ERR_VERIFY) {
        out("   %s was removed; start again to rewrite it\n", result.bad_file);
    } else if (err == XC_ERR_HTTP_DENIED && client.last_section != NULL) {
        out("   login missing or expired for [%s]\n", client.last_section);
    } else if (err == XC_ERR_HTTP_DENIED && !have_creds) {
        out("   no login was sent: no credentials were loaded from %s\n", CRED_FILE);
    } else if (client.last_status != 0 && (err == XC_ERR_HTTP_STATUS || err == XC_ERR_HTTP_DENIED ||
                                           err == XC_ERR_HTTP_NOT_FOUND)) {
        out("   HTTP %d\n", client.last_status);
    }
    if (result.files_written + result.files_skipped > 0) {
        out("   %u files are done in %s; start again to resume\n",
            (unsigned)(result.files_written + result.files_skipped), result.folder);
    }
    return false;
}

static bool install_url(const char *url)
{
    if (!network()) {
        out("\n%s\n  NOT INSTALLED: no network\n", url);
        return false;
    }
    if (is_https(url) && !https_ready()) {
        out("\n%s\n  NOT INSTALLED: HTTPS isn't available: %s\n", url, xc_err_str(https_err));
        return false;
    }
    static xc_http_source hs;
    xc_source src;
    xc_err err = xc_http_resource_init(&res, url, strlen(url));
    if (err == XC_OK) {
        err = xc_http_source_open(&src, &hs, &client, &res);
    }
    if (err == XC_ERR_UNSUPPORTED && !https_tried) {
        /* An http URL that redirected to https: set up TLS and try again. */
        err = https_ready() ? xc_http_source_open(&src, &hs, &client, &res) : https_err;
    }
    if (err != XC_OK) {
        out("\n%s\n  NOT INSTALLED: %s\n", url, xc_err_str(err));
        return false;
    }
    bool ok = install(&src, url, res.etag[0] != '\0' ? res.etag : NULL,
                      res.last_modified[0] != '\0' ? res.last_modified : NULL);
    if (is_https(url) && tls != NULL) {
        /* After redirects: the server the data came from (ADR 0005 asks to watch the suite). */
        xc_tls_info info;
        xc_tls_last_info(tls, &info);
        if (info.suite_name != NULL) {
            out("  TLS %u.%u, %s\n", (info.version >> 8) - 2U, (info.version & 0xFFU) - 1U,
                info.suite_name);
        }
    }
    xc_http_client_close(&client);
    return ok;
}

static bool install_file(const char *path)
{
    xc_nx_file f = {NULL};
    xc_source src;
    xc_err err = xc_nx_file_open(&src, &f, path);
    if (err != XC_OK) {
        out("\n%s\n  can't read it: %s\n", path, xc_err_str(err));
        return false;
    }
    bool ok = install(&src, path, NULL, NULL);
    xc_nx_file_close(&f);
    return ok;
}

/* If there is nothing to install, copies the bundled samples to E:\xcat\isos (M2). */
static void seed_samples(void)
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(SAMPLE_DIR "\\*.iso", &fd);
    if (h == INVALID_HANDLE_VALUE) {
        out("Nothing to install: put a map in " MAP_FILE " or images in " ISO_DIR "\n");
        return;
    }
    do {
        char from[XC_NX_PATH_MAX];
        char to[XC_NX_PATH_MAX];
        snprintf(from, sizeof from, SAMPLE_DIR "\\%s", fd.cFileName);
        snprintf(to, sizeof to, ISO_DIR "\\%s", fd.cFileName);
        out("Copying the bundled test image %s to the HDD\n", fd.cFileName);
        if (!CopyFileA(from, to, TRUE)) {
            out("  copy failed (error %lu)\n", (unsigned long)GetLastError());
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

/* The images and zips in E:\xcat\isos. */
static int list_local(char names[MAX_LOCAL][XC_NX_PATH_MAX])
{
    static const char *const patterns[] = {ISO_DIR "\\*.iso", ISO_DIR "\\*.zip"};
    int count = 0;
    for (size_t k = 0; k < sizeof patterns / sizeof patterns[0]; k++) {
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(patterns[k], &fd);
        if (h == INVALID_HANDLE_VALUE) {
            continue;
        }
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && count < MAX_LOCAL) {
                snprintf(names[count++], XC_NX_PATH_MAX, ISO_DIR "\\%s", fd.cFileName);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return count;
}

int main(void)
{
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    debugPrint("xiso-cartographer %s - install (M5b)\n", xc_version_string());
    xc_nx_clock(&clk);
    run_start_ms = now_ms();
    pads_init();
    debugPrint("Hold LT + RT + Back + Black to return to the dashboard.\n");
    io_buf = malloc(IO_BUF_SIZE);
    recv_buf = malloc(RECV_BUF_SIZE);
    if (io_buf == NULL || recv_buf == NULL || !xc_nx_mount_e()) {
        debugPrint("Startup failed (memory or E: drive)\n");
        halt();
    }
    if (!xc_nx_mkdirs(ISO_DIR) || !xc_nx_mkdirs(GAMES_DIR)) {
        debugPrint("Could not create " ISO_DIR " or " GAMES_DIR "\n");
        halt();
    }
    open_log();
    out("==== xiso-cartographer %s install ====\n", xc_version_string());
    load_credentials();

    static xc_map map;
    bool have_map = load_map(&map);
    static char local[MAX_LOCAL][XC_NX_PATH_MAX];
    int nlocal = list_local(local);
    if ((!have_map || map.entry_count == 0) && nlocal == 0) {
        seed_samples();
        nlocal = list_local(local);
    }

    int installed = 0;
    int total = 0;
    for (size_t i = 0; have_map && i < map.entry_count && !exit_combo(); i++, total++) {
        installed += install_url(map.entries[i].url) ? 1 : 0;
    }
    for (int i = 0; i < nlocal && !exit_combo(); i++, total++) {
        installed += install_file(local[i]) ? 1 : 0;
    }
    if (exit_combo()) {
        xc_map_free(&map);
        to_dashboard();
    }
    xc_map_free(&map);
    out("\nDone: %d of %d image(s) installed and verified.\n", installed, total);
    halt();
}
