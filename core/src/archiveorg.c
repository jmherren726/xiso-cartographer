/* archive.org item listings (xcat/archiveorg.h, docs/formats/archiveorg.md). */
#include "xcat/archiveorg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "text.h"

#define DOWNLOAD_PREFIX "https://archive.org/download/"
#define METADATA_PREFIX "https://archive.org/metadata/"

/* Names are decoded into a buffer this large; longer ones are invalid anyway (§3.1). */
#define NAME_BUF (4 * XC_ARCHIVEORG_MAX_NAME + 1)

bool xc_archiveorg_valid_id(const char *id)
{
    size_t n = strlen(id);
    if (n == 0 || n > XC_ARCHIVEORG_MAX_ID) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '_' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

xc_err xc_archiveorg_metadata_url(const char *id, char *out, size_t cap)
{
    if (!xc_archiveorg_valid_id(id)) {
        return XC_ERR_INVALID_ARG;
    }
    int n = snprintf(out, cap, "%s%s", METADATA_PREFIX, id);
    return n >= 0 && (size_t)n < cap ? XC_OK : XC_ERR_LIMIT;
}

/* The longest image or zip extension `name` ends with (§3 rule 1, §4), or 0. */
static size_t image_ext(const char *name, size_t len, bool *zip)
{
    static const char *const exts[] = {".xiso.iso", ".iso", ".xiso", ".zip"};
    for (size_t i = 0; i < sizeof exts / sizeof exts[0]; i++) {
        size_t n = strlen(exts[i]);
        if (len >= n && xc_text_eq_ci(name + len - n, exts[i])) {
            *zip = i == 3;
            return n;
        }
    }
    return 0;
}

/* §3.1 */
static bool valid_path(const char *p, size_t len)
{
    if (len == 0 || len > XC_ARCHIVEORG_MAX_NAME || p[0] == '/' || p[len - 1] == '/') {
        return false;
    }
    size_t seg = 0; /* start of the current segment */
    for (size_t i = 0; i <= len; i++) {
        unsigned char c = i < len ? (unsigned char)p[i] : '/';
        if (c < 0x20U || c == 0x7FU || c == '\\') {
            return false;
        }
        if (c != '/') {
            continue;
        }
        size_t n = i - seg;
        if (n == 0 || (n == 1 && p[seg] == '.') || (n == 2 && p[seg] == '.' && p[seg + 1] == '.')) {
            return false;
        }
        seg = i + 1;
    }
    return true;
}

/* §3.2: 1 to 2^63 - 1 in decimal, no leading zero. */
static bool parse_size(const char *s, size_t len, uint64_t *out)
{
    if (len == 0 || len > 19 || s[0] == '0') {
        return false;
    }
    uint64_t v = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
        uint64_t d = (uint64_t)(s[i] - '0');
        if (v > (UINT64_C(0x7FFFFFFFFFFFFFFF) - d) / 10U) {
            return false;
        }
        v = v * 10U + d;
    }
    *out = v;
    return true;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* §3.3: exactly 2 * n hex digits into out[0, n). */
static bool parse_hex(const char *s, size_t len, uint8_t *out, size_t n)
{
    if (len != 2 * n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        int hi = hex_value(s[2 * i]);
        int lo = hex_value(s[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((unsigned)hi << 4U | (unsigned)lo);
    }
    return true;
}

/* What one element of "files" said, before it is checked. */
typedef struct raw_file {
    char name[NAME_BUF];
    size_t name_len;
    bool has_name;
    bool name_bad; /* present, but not a string that decodes into the buffer */
    char size[24];
    bool has_size;
    bool size_bad;
    char md5[2 * XC_MD5_BYTES + 1];
    char sha1[2 * XC_SHA1_BYTES + 1];
    char crc32[9];
    int md5_state; /* 0 absent, 1 present, -1 malformed */
    int sha1_state;
    int crc32_state;
    bool source_ok;
    bool private_file;
} raw_file;

typedef struct parser {
    xc_json json;
    const char *id;
    xc_archiveorg_item *item;
    size_t cap;
    raw_file raw;
} parser;

/* Reads a short string value into buf; state 1 if it fits, -1 otherwise (null too). */
static xc_err short_string(xc_json *j, char *buf, size_t cap, int *state)
{
    xc_json_tok t;
    xc_err err = xc_json_next(j, &t);
    if (err != XC_OK) {
        return err;
    }
    *state = t.type == XC_JSON_STRING && xc_json_string(&t, buf, cap, NULL) == XC_OK ? 1 : -1;
    return xc_json_skip(j, &t);
}

static xc_err read_file_object(parser *ps)
{
    raw_file *r = &ps->raw;
    memset(r, 0, sizeof *r);
    r->source_ok = true; /* absent counts as original (§3 rule 2) */
    for (;;) {
        xc_json_tok key;
        xc_err err = xc_json_next(&ps->json, &key);
        if (err != XC_OK) {
            return err;
        }
        if (key.type == XC_JSON_OBJECT_END) {
            return XC_OK;
        }
        xc_json_tok v;
        if (xc_json_equals(&key, "name")) {
            err = xc_json_next(&ps->json, &v);
            if (err != XC_OK) {
                return err;
            }
            r->has_name = true;
            r->name_bad = v.type != XC_JSON_STRING ||
                          xc_json_string(&v, r->name, sizeof r->name, &r->name_len) != XC_OK;
            err = xc_json_skip(&ps->json, &v);
        } else if (xc_json_equals(&key, "size")) {
            err = xc_json_next(&ps->json, &v);
            if (err != XC_OK) {
                return err;
            }
            r->has_size = true;
            r->size_bad = true;
            if ((v.type == XC_JSON_STRING || v.type == XC_JSON_NUMBER) &&
                v.raw_len < sizeof r->size && !v.escaped) {
                memcpy(r->size, v.raw, v.raw_len);
                r->size[v.raw_len] = '\0';
                r->size_bad = false;
            }
            err = xc_json_skip(&ps->json, &v);
        } else if (xc_json_equals(&key, "md5")) {
            err = short_string(&ps->json, r->md5, sizeof r->md5, &r->md5_state);
        } else if (xc_json_equals(&key, "sha1")) {
            err = short_string(&ps->json, r->sha1, sizeof r->sha1, &r->sha1_state);
        } else if (xc_json_equals(&key, "crc32")) {
            err = short_string(&ps->json, r->crc32, sizeof r->crc32, &r->crc32_state);
        } else if (xc_json_equals(&key, "source") || xc_json_equals(&key, "private")) {
            bool source = xc_json_equals(&key, "source");
            err = xc_json_next(&ps->json, &v);
            if (err != XC_OK) {
                return err;
            }
            if (source) {
                r->source_ok = xc_json_equals(&v, "original");
            } else {
                r->private_file = xc_json_equals(&v, "true");
            }
            err = xc_json_skip(&ps->json, &v);
        } else {
            err = xc_json_next(&ps->json, &v);
            if (err == XC_OK) {
                err = xc_json_skip(&ps->json, &v);
            }
        }
        if (err != XC_OK) {
            return err;
        }
    }
}

static void note_invalid(xc_archiveorg_item *item, const char *name, size_t len)
{
    if (item->invalid++ > 0) {
        return;
    }
    size_t n = len < XC_ARCHIVEORG_MAX_NAME ? len : XC_ARCHIVEORG_MAX_NAME;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        item->first_invalid[i] = c < 0x20U || c == 0x7FU ? '?' : (char)c;
    }
    item->first_invalid[n] = '\0';
}

static char *dup_n(const char *s, size_t n)
{
    char *d = malloc(n + 1);
    if (d != NULL) {
        memcpy(d, s, n);
        d[n] = '\0';
    }
    return d;
}

/* §4: the download URL, every byte but A-Z a-z 0-9 - . _ ~ / percent-encoded. */
static char *download_url(const char *id, const char *path, size_t len)
{
    size_t prefix = strlen(DOWNLOAD_PREFIX) + strlen(id) + 1;
    char *url = malloc(prefix + 3 * len + 1);
    if (url == NULL) {
        return NULL;
    }
    (void)snprintf(url, prefix + 1, "%s%s/", DOWNLOAD_PREFIX, id);
    size_t n = prefix;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)path[i];
        bool keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '-' || c == '.' || c == '_' || c == '~' || c == '/';
        if (keep) {
            url[n++] = (char)c;
        } else {
            static const char hex[] = "0123456789ABCDEF";
            url[n++] = '%';
            url[n++] = hex[c >> 4U];
            url[n++] = hex[c & 0x0FU];
        }
    }
    url[n] = '\0';
    return url;
}

static void take_hashes(xc_archiveorg_item *item, const raw_file *r, xc_archiveorg_file *f)
{
    if (r->md5_state != 0) {
        f->has_md5 = r->md5_state > 0 && parse_hex(r->md5, strlen(r->md5), f->md5, XC_MD5_BYTES);
        item->hashes_dropped += f->has_md5 ? 0U : 1U;
    }
    if (r->sha1_state != 0) {
        f->has_sha1 =
            r->sha1_state > 0 && parse_hex(r->sha1, strlen(r->sha1), f->sha1, XC_SHA1_BYTES);
        item->hashes_dropped += f->has_sha1 ? 0U : 1U;
    }
    if (r->crc32_state != 0) {
        uint8_t b[4];
        f->has_crc32 = r->crc32_state > 0 && parse_hex(r->crc32, strlen(r->crc32), b, 4);
        f->crc32 = f->has_crc32 ? (uint32_t)b[0] << 24U | (uint32_t)b[1] << 16U |
                                      (uint32_t)b[2] << 8U | (uint32_t)b[3]
                                : 0U;
        item->hashes_dropped += f->has_crc32 ? 0U : 1U;
    }
}

/* Checks the element just read (§3) and adds it to the item if it's chosen. */
static xc_err choose(parser *ps)
{
    const raw_file *r = &ps->raw;
    xc_archiveorg_item *item = ps->item;
    const char *name = r->name;
    size_t len = r->name_len;
    if (!r->has_name || r->name_bad || !r->source_ok) {
        return XC_OK;
    }
    bool zip = false;
    size_t ext = image_ext(name, len, &zip);
    if (ext == 0) {
        return XC_OK;
    }
    const char *base = name;
    for (size_t i = 0; i < len; i++) {
        if (name[i] == '/') {
            base = name + i + 1;
        }
    }
    size_t base_len = (size_t)(name + len - base);
    uint64_t size = 0;
    if (!valid_path(name, len) || base_len <= ext || !r->has_size || r->size_bad ||
        !parse_size(r->size, strlen(r->size), &size)) {
        note_invalid(item, name, len);
        return XC_OK;
    }
    if (item->file_count == XC_ARCHIVEORG_MAX_FILES) {
        item->truncated = true;
        return XC_OK;
    }
    if (item->file_count == ps->cap) {
        size_t cap = ps->cap == 0 ? 16 : ps->cap * 2;
        xc_archiveorg_file *grown = realloc(item->files, cap * sizeof *grown);
        if (grown == NULL) {
            return XC_ERR_NOMEM;
        }
        item->files = grown;
        ps->cap = cap;
    }
    xc_archiveorg_file *f = &item->files[item->file_count];
    memset(f, 0, sizeof *f);
    f->path = dup_n(name, len);
    f->name = dup_n(base, base_len - ext);
    f->url = download_url(ps->id, name, len);
    item->file_count++; /* so xc_archiveorg_free sees it even if a copy failed */
    if (f->path == NULL || f->name == NULL || f->url == NULL) {
        return XC_ERR_NOMEM;
    }
    f->size = size;
    f->zip = zip;
    f->private_file = r->private_file;
    take_hashes(item, r, f);
    return XC_OK;
}

static xc_err read_files(parser *ps)
{
    for (;;) {
        xc_json_tok t;
        xc_err err = xc_json_next(&ps->json, &t);
        if (err != XC_OK) {
            return err;
        }
        if (t.type == XC_JSON_ARRAY_END) {
            return XC_OK;
        }
        if (t.type != XC_JSON_OBJECT) {
            err = xc_json_skip(&ps->json, &t); /* §2: not an object */
        } else {
            err = read_file_object(ps);
            if (err == XC_OK) {
                err = choose(ps);
            }
        }
        if (err != XC_OK) {
            return err;
        }
    }
}

/* XC_ERR_CORRUPT when the body isn't item metadata (§2). */
static xc_err read_body(parser *ps, bool *dark, bool *has_files)
{
    xc_json_tok t;
    xc_err err = xc_json_next(&ps->json, &t);
    if (err != XC_OK || t.type != XC_JSON_OBJECT) {
        return err == XC_ERR_NOMEM ? err : XC_ERR_CORRUPT;
    }
    for (;;) {
        xc_json_tok key;
        err = xc_json_next(&ps->json, &key);
        if (err != XC_OK || key.type == XC_JSON_OBJECT_END) {
            break;
        }
        xc_json_tok v;
        err = xc_json_next(&ps->json, &v);
        if (err != XC_OK) {
            break;
        }
        if (xc_json_equals(&key, "files")) {
            if (v.type != XC_JSON_ARRAY || *has_files) {
                return XC_ERR_CORRUPT;
            }
            *has_files = true;
            err = read_files(ps);
        } else {
            if (xc_json_equals(&key, "is_dark")) {
                *dark = v.type == XC_JSON_TRUE;
            }
            err = xc_json_skip(&ps->json, &v);
        }
        if (err != XC_OK) {
            break;
        }
    }
    if (err == XC_OK) {
        err = xc_json_next(&ps->json, &t);
        if (err == XC_OK && t.type != XC_JSON_END) {
            err = XC_ERR_CORRUPT;
        }
    }
    return err == XC_OK || err == XC_ERR_NOMEM ? err : XC_ERR_CORRUPT;
}

static void free_files(xc_archiveorg_item *item)
{
    for (size_t i = 0; i < item->file_count; i++) {
        free(item->files[i].path);
        free(item->files[i].name);
        free(item->files[i].url);
    }
    free(item->files);
    item->files = NULL;
    item->file_count = 0;
}

xc_err xc_archiveorg_parse(const char *id, const char *body, size_t len, xc_archiveorg_item *item)
{
    memset(item, 0, sizeof *item);
    if (!xc_archiveorg_valid_id(id) || (body == NULL && len > 0)) {
        return XC_ERR_INVALID_ARG;
    }
    if (len > XC_ARCHIVEORG_MAX_METADATA) {
        return XC_ERR_LIMIT;
    }
    parser *ps = malloc(sizeof *ps);
    if (ps == NULL) {
        return XC_ERR_NOMEM;
    }
    xc_json_init(&ps->json, body != NULL ? body : "", len);
    ps->id = id;
    ps->item = item;
    ps->cap = 0;
    bool dark = false;
    bool has_files = false;
    xc_err err = read_body(ps, &dark, &has_files);
    free(ps);
    if (err == XC_ERR_NOMEM) {
        free_files(item);
        return err;
    }
    if (err != XC_OK) {
        item->status = XC_ARCHIVEORG_NOT_METADATA;
    } else if (dark) {
        item->status = XC_ARCHIVEORG_DARK;
    } else if (!has_files) {
        item->status = XC_ARCHIVEORG_MISSING;
    } else {
        item->status = XC_ARCHIVEORG_OK;
    }
    if (item->status != XC_ARCHIVEORG_OK) {
        free_files(item);
        item->invalid = 0;
        item->first_invalid[0] = '\0';
        item->hashes_dropped = 0;
        item->truncated = false;
    }
    return XC_OK;
}

void xc_archiveorg_free(xc_archiveorg_item *item)
{
    free_files(item);
}
