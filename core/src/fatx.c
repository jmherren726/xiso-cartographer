/*
 * FATX naming rules (docs/formats/fatx-names.md; "§n" below refers to it) and the
 * install checks built on them (ADR 0008).
 */
#include "xcat/fatx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * §2: bytes the kernel accepts in a name, as bit (c % 32) of word (c / 32). Measured
 * by app/fatx-probe v2 on kernel 1.0.4627.1 (xemu; the console matched). 0x00 and
 * 0x5C ('\') were not testable and are rejected. test_fatx.c checks this table
 * against the measured list, byte for byte.
 */
static const uint32_t allowed[8] = {
    0x00000000U, 0x03FF63FBU, 0xEFFFFFFFU, 0xEFFFFFFFU,
    0x7FFD752FU, 0x7FFE9998U, 0x7FFE9998U, 0x7FFD7D0CU,
};

bool xc_fatx_byte_ok(uint8_t c)
{
    return ((allowed[c >> 5U] >> (c & 31U)) & 1U) != 0;
}

/* §1, §2 */
xc_fatx_name_check xc_fatx_check_name(const char *name, size_t len, size_t *bad_index)
{
    if (name == NULL || len == 0) {
        return XC_FATX_NAME_EMPTY;
    }
    if ((len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.')) {
        return XC_FATX_NAME_DOTS;
    }
    if (len > XC_FATX_MAX_NAME) {
        return XC_FATX_NAME_TOO_LONG;
    }
    for (size_t i = 0; i < len; i++) {
        if (!xc_fatx_byte_ok((uint8_t)name[i])) {
            if (bad_index != NULL) {
                *bad_index = i;
            }
            return XC_FATX_NAME_BAD_BYTE;
        }
    }
    return XC_FATX_NAME_OK;
}

/* --- listing check ------------------------------------------------------------- */

typedef struct parent_ref {
    const char *p;
    size_t len;
} parent_ref;

static int parent_cmp(const void *a, const void *b)
{
    const parent_ref *x = a;
    const parent_ref *y = b;
    if (x->len != y->len) {
        return x->len < y->len ? -1 : 1;
    }
    return memcmp(x->p, y->p, x->len);
}

typedef struct problems {
    xc_fatx_problem *out;
    size_t max;
    size_t count;
} problems;

static void add(problems *pr, xc_fatx_problem_kind kind, xc_fatx_name_check name, const char *path,
                size_t value)
{
    if (pr->count < pr->max) {
        xc_fatx_problem *p = &pr->out[pr->count];
        p->kind = kind;
        p->name = name;
        p->path = path;
        p->value = value;
    }
    pr->count++;
}

static void check_entry(problems *pr, const xc_xdvdfs_entry *e, size_t install_dir_len)
{
    size_t plen = strlen(e->path);
    const char *slash = strrchr(e->path, '/');
    const char *name = slash != NULL ? slash + 1 : e->path;
    size_t bad = 0;
    xc_fatx_name_check nc = xc_fatx_check_name(name, plen - (size_t)(name - e->path), &bad);
    if (nc != XC_FATX_NAME_OK) {
        add(pr, XC_FATX_PROBLEM_NAME, nc, e->path, nc == XC_FATX_NAME_BAD_BYTE ? bad : 0);
    }
    /* §3: "\Games\Title" + "\" + path, with '/' becoming '\' (same length). */
    size_t full = install_dir_len + 1 + plen;
    if (full > XC_FATX_MAX_PATH) {
        add(pr, XC_FATX_PROBLEM_PATH_TOO_LONG, XC_FATX_NAME_OK, e->path, full);
    }
    /* §4b: only files have a size here (a directory's is its table, never near the limit). */
    if ((e->attributes & 0x10U) == 0 && e->size > XC_FATX_MAX_FILE_SIZE) {
        add(pr, XC_FATX_PROBLEM_FILE_TOO_BIG, XC_FATX_NAME_OK, e->path, e->size);
    }
}

/* The listing path of directory `p` (len bytes), or "" for the install root. */
static const char *dir_path(const xc_xdvdfs_listing *l, const char *p, size_t len)
{
    if (len == 0) {
        return "";
    }
    for (size_t i = 0; i < l->dir_count; i++) {
        if (strlen(l->dirs[i].path) == len && memcmp(l->dirs[i].path, p, len) == 0) {
            return l->dirs[i].path;
        }
    }
    return p; /* can't happen for a listing from xc_xdvdfs_list(); still a valid pointer */
}

xc_err xc_fatx_check_listing(const xc_xdvdfs_listing *listing, size_t install_dir_len,
                             xc_fatx_problem *problems_out, size_t max_problems,
                             size_t *problem_count)
{
    if (listing == NULL || problem_count == NULL || (problems_out == NULL && max_problems > 0) ||
        install_dir_len > XC_FATX_MAX_PATH) {
        return XC_ERR_INVALID_ARG;
    }
    problems pr = {problems_out, max_problems, 0};
    for (size_t i = 0; i < listing->dir_count; i++) {
        check_entry(&pr, &listing->dirs[i], install_dir_len);
    }
    for (size_t i = 0; i < listing->file_count; i++) {
        check_entry(&pr, &listing->files[i], install_dir_len);
    }

    /* §4: count the entries in each directory by grouping every entry's parent path. */
    size_t n = listing->dir_count + listing->file_count;
    if (n > 0) {
        if (n > SIZE_MAX / sizeof(parent_ref)) {
            return XC_ERR_NOMEM;
        }
        parent_ref *parents = malloc(n * sizeof *parents);
        if (parents == NULL) {
            return XC_ERR_NOMEM;
        }
        for (size_t i = 0; i < n; i++) {
            const char *path = i < listing->dir_count ? listing->dirs[i].path
                                                      : listing->files[i - listing->dir_count].path;
            const char *slash = strrchr(path, '/');
            parents[i].p = path;
            parents[i].len = slash != NULL ? (size_t)(slash - path) : 0;
        }
        qsort(parents, n, sizeof *parents, parent_cmp);
        size_t run = 1;
        for (size_t i = 1; i <= n; i++) {
            if (i < n && parent_cmp(&parents[i - 1], &parents[i]) == 0) {
                run++;
                continue;
            }
            if (run > XC_FATX_MAX_DIR_ENTRIES) {
                add(&pr, XC_FATX_PROBLEM_TOO_MANY_ENTRIES, XC_FATX_NAME_OK,
                    dir_path(listing, parents[i - 1].p, parents[i - 1].len), run);
            }
            run = 1;
        }
        free(parents);
    }
    *problem_count = pr.count;
    return XC_OK;
}

/* --- folder names ---------------------------------------------------------------- */

/* Printable ASCII the kernel accepts; anything else in a title is replaced. */
static bool title_byte_ok(uint8_t c)
{
    return c >= 0x20 && c < 0x7F && xc_fatx_byte_ok(c);
}

static bool trim_byte(char c)
{
    return c == ' ' || c == '.' || c == '_';
}

/* Removes trim bytes from both ends of out[0..n). Returns the new length. */
static size_t trim(char *out, size_t n)
{
    size_t start = 0;
    while (start < n && trim_byte(out[start])) {
        start++;
    }
    while (n > start && trim_byte(out[n - 1])) {
        n--;
    }
    if (start > 0) {
        memmove(out, out + start, n - start);
    }
    return n - start;
}

size_t xc_fatx_title_name(const char *title, char *out, size_t max_len)
{
    if (max_len < 1) {
        max_len = 1;
    }
    if (max_len > XC_FATX_MAX_NAME) {
        max_len = XC_FATX_MAX_NAME;
    }
    size_t n = 0;
    for (size_t i = 0; title != NULL && title[i] != '\0' && n < max_len; i++) {
        uint8_t c = (uint8_t)title[i];
        if (c == ':' && title[i + 1] == ' ') {
            /* "Halo: Combat Evolved" -> "Halo - Combat Evolved" */
            if (n == 0 || out[n - 1] != ' ') {
                out[n++] = ' ';
            }
            if (n < max_len) {
                out[n++] = '-';
            }
        } else if (title_byte_ok(c)) {
            out[n++] = (char)c;
        } else if (n == 0 || out[n - 1] != '_') {
            out[n++] = '_'; /* one '_' per run of invalid bytes, e.g. a UTF-8 sequence */
        }
    }
    n = trim(out, n);
    if (n == 0) {
        static const char fallback[] = "Game";
        n = sizeof fallback - 1 < max_len ? sizeof fallback - 1 : max_len;
        memcpy(out, fallback, n);
    }
    out[n] = '\0';
    return n;
}

size_t xc_fatx_numbered_name(const char *base, unsigned n, char *out)
{
    char suffix[16];
    int s = snprintf(suffix, sizeof suffix, " (%u)", n);
    if (s <= 0 || (size_t)s >= sizeof suffix) {
        s = 0;
        suffix[0] = '\0';
    }
    size_t keep = base != NULL ? strlen(base) : 0;
    if (keep > XC_FATX_MAX_NAME - (size_t)s) {
        keep = XC_FATX_MAX_NAME - (size_t)s;
    }
    while (keep > 0 && (base[keep - 1] == ' ' || base[keep - 1] == '.')) {
        keep--; /* "Some Title (2)", not "Some Title  (2)" after truncation */
    }
    int w = snprintf(out, XC_FATX_MAX_NAME + 1, "%.*s%s", (int)keep, keep > 0 ? base : "", suffix);
    return w > 0 ? (size_t)w : 0;
}
