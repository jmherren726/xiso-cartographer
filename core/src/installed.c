/*
 * What's already in the games folder (xcat/installed.h): the marker of
 * docs/formats/journal.md §7.1, and the folder names of ADR 0008.
 */
#include "xcat/installed.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "text.h"
#include "xcat/journal.h"

#define PROGRAM "xiso-cartographer"

/* Copies v[0, n) into out (of `size`), cut at a UTF-8 boundary if it doesn't fit. False, with
   out empty, if v isn't UTF-8 text or holds a control character. */
static bool copy_text(char *out, size_t size, const char *v, size_t n)
{
    out[0] = '\0';
    size_t bad = 0;
    if (!xc_text_utf8((const unsigned char *)v, n, &bad)) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)v[i];
        if (c < 0x20U || c == 0x7FU) {
            return false;
        }
    }
    if (n >= size) {
        n = size - 1;
        while (n > 0 && ((unsigned char)v[n] & 0xC0U) == 0x80U) {
            n--; /* v[n] continues a sequence: cut before the sequence starts */
        }
    }
    memcpy(out, v, n);
    out[n] = '\0';
    return true;
}

/* A decimal number without sign, at most 20 digits and no overflow. */
static bool parse_u64(const char *v, size_t n, uint64_t *out)
{
    *out = 0;
    if (n == 0 || n > 20) {
        return false;
    }
    uint64_t x = 0;
    for (size_t i = 0; i < n; i++) {
        if (v[i] < '0' || v[i] > '9') {
            return false;
        }
        uint64_t d = (uint64_t)(v[i] - '0');
        if (x > (UINT64_MAX - d) / 10U) {
            return false;
        }
        x = x * 10U + d;
    }
    *out = x;
    return true;
}

static bool is_hex(const char *v, size_t n, size_t want)
{
    if (n != want) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = v[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

static bool key_is(const char *k, size_t kn, const char *name)
{
    return strlen(name) == kn && memcmp(k, name, kn) == 0;
}

static void take(xc_marker *m, const char *k, size_t kn, const char *v, size_t vn)
{
    if (key_is(k, kn, "image")) {
        (void)copy_text(m->image, sizeof m->image, v, vn);
    } else if (key_is(k, kn, "files")) {
        m->has_files = parse_u64(v, vn, &m->files);
    } else if (key_is(k, kn, "bytes")) {
        m->has_bytes = parse_u64(v, vn, &m->bytes);
    } else if (key_is(k, kn, "verified")) {
        (void)copy_text(m->verified, sizeof m->verified, v, vn);
    } else if (key_is(k, kn, "md5")) {
        (void)copy_text(m->md5, sizeof m->md5, v, is_hex(v, vn, sizeof m->md5 - 1) ? vn : 0);
    } else if (key_is(k, kn, "sha1")) {
        (void)copy_text(m->sha1, sizeof m->sha1, v, is_hex(v, vn, sizeof m->sha1 - 1) ? vn : 0);
    } else if (key_is(k, kn, "title_id")) {
        m->has_title_id = is_hex(v, vn, 8);
        m->title_id = 0;
        for (size_t i = 0; m->has_title_id && i < vn; i++) {
            unsigned c = (unsigned char)v[i];
            unsigned d = c <= '9' ? c - '0' : (c | 0x20U) - 'a' + 10U;
            m->title_id = m->title_id << 4U | d;
        }
    } else if (key_is(k, kn, "title")) {
        (void)copy_text(m->title, sizeof m->title, v, vn);
    }
}

xc_err xc_marker_parse(const char *data, size_t len, xc_marker *m)
{
    if (m == NULL || (data == NULL && len > 0)) {
        return XC_ERR_INVALID_ARG;
    }
    *m = (xc_marker){0};
    if (len > XC_MARKER_MAX_FILE) {
        return XC_ERR_LIMIT;
    }
    if (len == 0 || memchr(data, '\0', len) != NULL) {
        return XC_ERR_CORRUPT;
    }
    const char *end = data + len;
    bool first = true;
    for (const char *p = data; p < end;) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *eol = nl != NULL ? nl : end;
        size_t n = (size_t)(eol - p);
        if (n > 0 && p[n - 1] == '\r') {
            n--;
        }
        if (first) {
            size_t pn = strlen(PROGRAM);
            if (n < pn || memcmp(p, PROGRAM, pn) != 0 || (n > pn && p[pn] != ' ')) {
                return XC_ERR_CORRUPT;
            }
            (void)copy_text(m->program, sizeof m->program, p, n);
            first = false;
        } else {
            const char *eq = memchr(p, '=', n);
            if (eq != NULL) {
                take(m, p, (size_t)(eq - p), eq + 1, n - (size_t)(eq - p) - 1);
            }
        }
        p = nl != NULL ? nl + 1 : end;
    }
    return XC_OK;
}

static bool join(char *out, size_t size, char sep, const char *a, const char *b)
{
    int n = snprintf(out, size, "%s%c%s", a, sep, b);
    return n >= 0 && (size_t)n < size;
}

xc_err xc_installed_check(const xc_install_io *io, const char *games_dir, const char *folder,
                          xc_installed *out)
{
    if (io == NULL || games_dir == NULL || folder == NULL || out == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    *out = (xc_installed){0};
    char dir[XC_INSTALL_MAX_PATH];
    char path[XC_INSTALL_MAX_PATH];
    if (!join(dir, sizeof dir, io->separator, games_dir, folder) ||
        !join(path, sizeof path, io->separator, dir, XC_INSTALL_MARKER)) {
        return XC_ERR_LIMIT;
    }
    if (io->exists(io->ctx, path)) {
        out->state = XC_FOLDER_INSTALLED;
        char *data = NULL;
        size_t len = 0;
        if (io->read_all(io->ctx, path, XC_MARKER_MAX_FILE, &data, &len) == XC_OK) {
            out->marker_ok = xc_marker_parse(data, len, &out->marker) == XC_OK;
        }
        free(data);
        return XC_OK;
    }
    if (!join(path, sizeof path, io->separator, dir, XC_JOURNAL_NAME)) {
        return XC_ERR_LIMIT;
    }
    out->state = io->exists(io->ctx, path) ? XC_FOLDER_UNFINISHED : XC_FOLDER_OTHER;
    return XC_OK;
}

bool xc_installed_folder_for(const char *folder, const char *title)
{
    if (folder == NULL || title == NULL) {
        return false;
    }
    char base[XC_FATX_MAX_NAME + 1];
    char name[XC_FATX_MAX_NAME + 1];
    (void)xc_fatx_title_name(title, base, XC_FATX_MAX_NAME);
    if (xc_text_eq_ci(folder, base)) {
        return true;
    }
    for (unsigned k = 2; k <= XC_INSTALL_MAX_NUMBERED; k++) {
        (void)xc_fatx_numbered_name(base, k, name);
        if (xc_text_eq_ci(folder, name)) {
            return true;
        }
    }
    return false;
}
