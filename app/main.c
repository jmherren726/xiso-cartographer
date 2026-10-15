/*
 * xiso-cartographer XBE, milestone M4: install games over the network, resumably.
 *
 * What it installs, in this order:
 *   1. every entry of the map E:\xcat\map.txt (docs/formats/map.md), downloaded over HTTP
 *      with Range and installed while it streams in (ADR 0004);
 *   2. every image in E:\xcat\isos\ (the M2 local install);
 *   3. if there is neither, the bundled synthetic test image (D:\samples\*.iso).
 * Each one goes through xc_install (xcat/install.h): FATX check, free space, a journal so a
 * pulled cable or a power cut resumes instead of starting over, a read-back check of every
 * file, and the .xcat-installed marker. Logins come from E:\Apps\xcat\credentials
 * (docs/formats/credentials.md). Everything shown is also appended to E:\xcat\install.log.
 * The real UI replaces this in M8.
 */
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

/* "12.3" from tenths: PDCLib's printf has no floating point. */
static void tenths(char *buf, size_t size, uint64_t t)
{
    (void)snprintf(buf, size, "%llu.%llu", (unsigned long long)(t / 10U),
                   (unsigned long long)(t % 10U));
}

/* --- network, on first use ------------------------------------------------------------ */

static bool net_tried;
static bool net_up;
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
    return true;
}

static void load_credentials(void)
{
    if (!xc_nx_exists(CRED_FILE)) {
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
    xc_http_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.net = &net;
    cfg.clock = &clk;
    cfg.buf = recv_buf;
    cfg.buf_size = RECV_BUF_SIZE;
    cfg.max_attempts = ATTEMPTS;
    cfg.seed = (uint32_t)now_ms();
    cfg.on_retry = on_retry;
    if (have_creds) {
        xc_cred_http_auth(&auth, &creds);
        cfg.auth = &auth;
    }
    net_up = xc_http_client_init(&client, &cfg) == XC_OK;
    return net_up;
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
} mem_ctx;

static xc_err to_mem(void *ctx, uint64_t offset, const uint8_t *data, size_t len)
{
    memcpy(((mem_ctx *)ctx)->buf + (size_t)offset, data, len);
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
    xc_err err = xc_http_resource_init(&res, location, strlen(location));
    if (err == XC_OK) {
        err = xc_http_probe(&client, &res);
    }
    if (err == XC_OK && res.size > max_len) {
        err = XC_ERR_LIMIT;
    }
    mem_ctx m = {NULL};
    if (err == XC_OK) {
        m.buf = malloc((size_t)res.size + 1);
        err = m.buf != NULL ? xc_http_read(&client, &res, 0, res.size, to_mem, &m) : XC_ERR_NOMEM;
    }
    if (err != XC_OK) {
        free(m.buf);
        return err;
    }
    *data = m.buf;
    *len = (size_t)res.size;
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

/* "dir/Halo%20(USA).xiso.iso?x" -> "Halo (USA)" */
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
    if (ends_with_ci(out_title, n, ".iso")) {
        n -= 4;
    }
    if (ends_with_ci(out_title, n, ".xiso")) {
        n -= 5;
    }
    out_title[n] = '\0';
}

typedef struct ui {
    uint64_t start_ms;
    uint64_t next_ms;
    int phase;
} ui;

static bool on_progress(void *ctx, const xc_install_progress *p)
{
    ui *u = ctx;
    uint64_t now = now_ms();
    if ((int)p->phase != u->phase) {
        u->phase = (int)p->phase;
        u->next_ms = 0;
        if (p->phase == XC_INSTALL_WRITING) {
            u->start_ms = now;
        }
        out("  %s\n", p->phase == XC_INSTALL_WRITING     ? "writing files"
                      : p->phase == XC_INSTALL_VERIFYING ? "verifying (reading every file back)"
                                                         : "reading the directory tables");
    }
    if (now >= u->next_ms && p->bytes_total > 0) {
        u->next_ms = now + REPORT_EVERY_MS;
        char speed[24] = "-";
        if (p->phase == XC_INSTALL_WRITING && now > u->start_ms) {
            tenths(speed, sizeof speed, p->bytes_done / (now - u->start_ms) / 100U);
        }
        out("  %4llu s  %llu of %llu MiB, file %u of %u, %s MB/s\n",
            (unsigned long long)((now - run_start_ms) / 1000U),
            (unsigned long long)(p->bytes_done >> 20U), (unsigned long long)(p->bytes_total >> 20U),
            (unsigned)p->files_done, (unsigned)p->file_count, speed);
    }
    return true;
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
    }
    return "problem";
}

static xc_install_result result;

/* Installs one image; true if it ended installed and verified. */
static bool install(const xc_source *src, const char *name, const char *etag,
                    const char *last_modified)
{
    char title[128];
    title_from(name, title, sizeof title);
    out("\n%s\n", name);
    xc_install_io io;
    xc_nx_install_io(&io);
    ui u = {now_ms(), 0, -1};
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
    xc_err err = xc_install(&req, &result);
    uint64_t ms = now_ms() - u.start_ms;
    if (result.other_folder_reason != NULL) {
        out("  (not the existing folder of that name: %s)\n", result.other_folder_reason);
    }
    if (err == XC_OK) {
        char speed[24];
        tenths(speed, sizeof speed, ms > 0 ? result.bytes_written / ms / 100U : 0);
        out("  OK: %u files in %s, verified%s\n", (unsigned)result.file_count, result.folder,
            result.resumed ? " (resumed)" : "");
        out("  %u written, %u kept from an earlier attempt; %llu MiB in %llu s (%s MB/s)\n",
            (unsigned)result.files_written, (unsigned)result.files_skipped,
            (unsigned long long)(result.bytes_written >> 20U), (unsigned long long)(ms / 1000U),
            speed);
        return true;
    }
    out("  NOT INSTALLED: %s\n", xc_err_str(err));
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
    static xc_http_source hs;
    xc_source src;
    xc_err err = xc_http_resource_init(&res, url, strlen(url));
    if (err == XC_OK) {
        err = xc_http_source_open(&src, &hs, &client, &res);
    }
    if (err != XC_OK) {
        out("\n%s\n  NOT INSTALLED: %s\n", url, xc_err_str(err));
        return false;
    }
    bool ok = install(&src, url, res.etag[0] != '\0' ? res.etag : NULL,
                      res.last_modified[0] != '\0' ? res.last_modified : NULL);
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

static int list_local(char names[MAX_LOCAL][XC_NX_PATH_MAX])
{
    int count = 0;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(ISO_DIR "\\*.iso", &fd);
    if (h != INVALID_HANDLE_VALUE) {
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
    debugPrint("xiso-cartographer %s - install (M4)\n", xc_version_string());
    xc_nx_clock(&clk);
    run_start_ms = now_ms();
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
    for (size_t i = 0; have_map && i < map.entry_count; i++, total++) {
        installed += install_url(map.entries[i].url) ? 1 : 0;
    }
    for (int i = 0; i < nlocal; i++, total++) {
        installed += install_file(local[i]) ? 1 : 0;
    }
    xc_map_free(&map);
    out("\nDone: %d of %d image(s) installed and verified.\n", installed, total);
    halt();
}
