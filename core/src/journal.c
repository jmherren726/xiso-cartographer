/*
 * Install journal: docs/formats/journal.md §3 (syntax), §4 (identity), §5
 * (records) and §6 (reading: stop at a torn line, reject anything a power cut
 * can't explain).
 */
#include "xcat/journal.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "text.h"
#include "xcat/crc32.h"

/* --- writing ----------------------------------------------------------------------------- */

typedef struct wbuf {
    char *p;
    size_t cap;
    size_t n;
    size_t line_start;
    bool overflow;
} wbuf;

static void w_mem(wbuf *w, const char *s, size_t len)
{
    if (w->overflow || len > w->cap - w->n) {
        w->overflow = true;
        return;
    }
    memcpy(w->p + w->n, s, len);
    w->n += len;
}

static void w_str(wbuf *w, const char *s)
{
    w_mem(w, s, strlen(s));
}

static void w_fmt(wbuf *w, const char *fmt, uint64_t v)
{
    char tmp[32];
    int n = snprintf(tmp, sizeof tmp, fmt, v);
    if (n > 0) {
        w_mem(w, tmp, (size_t)n);
    }
}

static void w_hex32(wbuf *w, uint32_t v)
{
    char tmp[16];
    (void)snprintf(tmp, sizeof tmp, "%08" PRIX32, v);
    w_mem(w, tmp, 8);
}

/* §3.4: bytes below 0x21, above 0x7E, and '%' become %XX. */
static void w_enc(wbuf *w, const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
        if (*p < 0x21U || *p > 0x7EU || *p == '%') {
            char esc[3] = {'%', hex[*p >> 4U], hex[*p & 0xFU]};
            w_mem(w, esc, 3);
        } else {
            w_mem(w, (const char *)p, 1);
        }
    }
}

/* A validator: encoded, or "-" for none; a literal "-" is written %2D (§4). */
static void w_validator(wbuf *w, const char *v)
{
    if (v[0] == '\0') {
        w_str(w, "-");
    } else if (strcmp(v, "-") == 0) {
        w_str(w, "%2D");
    } else {
        w_enc(w, v);
    }
}

/* §3.2: " *<CRC of the line so far>\n". */
static void w_end_line(wbuf *w)
{
    if (w->overflow || w->n - w->line_start > XC_JOURNAL_MAX_LINE - 10U) {
        w->overflow = true;
        return;
    }
    uint32_t crc = xc_crc32(0, w->p + w->line_start, w->n - w->line_start);
    w_str(w, " *");
    w_hex32(w, crc);
    w_str(w, "\n");
    w->line_start = w->n;
}

static xc_err w_result(const wbuf *w, size_t *len)
{
    *len = w->overflow ? 0 : w->n;
    return w->overflow ? XC_ERR_LIMIT : XC_OK;
}

uint32_t xc_journal_listing_crc(const xc_xdvdfs_listing *listing)
{
    uint32_t crc = 0;
    char num[48];
    for (size_t i = 0; i < listing->file_count; i++) {
        const xc_xdvdfs_entry *e = &listing->files[i];
        int n = snprintf(num, sizeof num, "%" PRIu64 " %" PRIu32 " ", e->offset, e->size);
        crc = xc_crc32(crc, num, (size_t)n);
        crc = xc_crc32(crc, e->path, strlen(e->path));
        crc = xc_crc32(crc, "\n", 1);
    }
    return crc;
}

static void copy_validator(char *dst, const char *src)
{
    dst[0] = '\0';
    if (src != NULL && strlen(src) <= XC_JOURNAL_MAX_VALIDATOR) {
        memcpy(dst, src, strlen(src) + 1);
    }
}

void xc_journal_make_id(xc_journal_id *id, const xc_xdvdfs_listing *listing,
                        uint64_t partition_offset, uint64_t image_size, const char *etag,
                        const char *last_modified)
{
    memset(id, 0, sizeof *id);
    id->image_size = image_size;
    id->partition_offset = partition_offset;
    id->listing_crc = xc_journal_listing_crc(listing);
    id->file_count = listing->file_count;
    copy_validator(id->etag, etag);
    copy_validator(id->last_modified, last_modified);
}

static bool validator_same(const char *a, const char *b)
{
    return a[0] == '\0' || b[0] == '\0' || strcmp(a, b) == 0;
}

bool xc_journal_same_image(const xc_journal_id *a, const xc_journal_id *b)
{
    return a->image_size == b->image_size && a->partition_offset == b->partition_offset &&
           a->listing_crc == b->listing_crc && a->file_count == b->file_count &&
           validator_same(a->etag, b->etag) && validator_same(a->last_modified, b->last_modified);
}

xc_err xc_journal_format_start(const xc_journal_id *id, const char *source_url, char *out,
                               size_t cap, size_t *len)
{
    if (id == NULL || out == NULL || len == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    wbuf w = {out, cap, 0, 0, false};
    w_str(&w, "# xcat-journal v1.2");
    w_end_line(&w);
    w_str(&w, "image ");
    w_fmt(&w, "%" PRIu64, id->image_size);
    w_str(&w, " ");
    w_fmt(&w, "%" PRIu64, id->partition_offset);
    w_str(&w, " ");
    w_hex32(&w, id->listing_crc);
    w_str(&w, " ");
    w_fmt(&w, "%" PRIu64, id->file_count);
    w_end_line(&w);
    w_str(&w, "validators ");
    w_validator(&w, id->etag);
    w_str(&w, " ");
    w_validator(&w, id->last_modified);
    w_end_line(&w);
    w_str(&w, "source ");
    w_enc(&w, source_url != NULL && source_url[0] != '\0' ? source_url : "-");
    w_end_line(&w);
    return w_result(&w, len);
}

xc_err xc_journal_format_done(const char *path, uint32_t size, uint32_t crc, char *out, size_t cap,
                              size_t *len)
{
    if (path == NULL || path[0] == '\0' || out == NULL || len == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    wbuf w = {out, cap, 0, 0, false};
    w_str(&w, "done ");
    w_fmt(&w, "%" PRIu64, size);
    w_str(&w, " ");
    w_hex32(&w, crc);
    w_str(&w, " ");
    w_enc(&w, path);
    w_end_line(&w);
    return w_result(&w, len);
}

xc_err xc_journal_format_part(const char *path, uint32_t bytes, uint32_t crc, char *out, size_t cap,
                              size_t *len)
{
    if (path == NULL || path[0] == '\0' || bytes == 0 || out == NULL || len == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    wbuf w = {out, cap, 0, 0, false};
    w_str(&w, "part ");
    w_fmt(&w, "%" PRIu64, bytes);
    w_str(&w, " ");
    w_hex32(&w, crc);
    w_str(&w, " ");
    w_enc(&w, path);
    w_end_line(&w);
    return w_result(&w, len);
}

static void w_hex_bytes(wbuf *w, const uint8_t *b, size_t n)
{
    static const char digits[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; i++) {
        char two[2] = {digits[b[i] >> 4U], digits[b[i] & 0xFU]};
        w_mem(w, two, 2);
    }
}

xc_err xc_journal_format_hash(uint64_t offset, const uint8_t md5_state[XC_MD5_BYTES],
                              const uint8_t sha1_state[XC_SHA1_BYTES], char *out, size_t cap,
                              size_t *len)
{
    if (md5_state == NULL || sha1_state == NULL || offset % XC_HASH_BLOCK != 0 || out == NULL ||
        len == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    wbuf w = {out, cap, 0, 0, false};
    w_str(&w, "hash ");
    w_fmt(&w, "%" PRIu64, offset);
    w_str(&w, " ");
    w_hex_bytes(&w, md5_state, XC_MD5_BYTES);
    w_str(&w, " ");
    w_hex_bytes(&w, sha1_state, XC_SHA1_BYTES);
    w_end_line(&w);
    return w_result(&w, len);
}

/* --- reading ----------------------------------------------------------------------------- */

/* Splits a record into fields at single spaces. Returns the count, or 0 if malformed. */
static size_t split_fields(char *s, char **fields, size_t max)
{
    size_t n = 0;
    for (;;) {
        if (*s == '\0' || *s == ' ' || n == max) {
            return 0; /* empty field (double space, leading/trailing space) or too many */
        }
        fields[n++] = s;
        char *sp = strchr(s, ' ');
        if (sp == NULL) {
            return n;
        }
        *sp = '\0';
        s = sp + 1;
    }
}

/* §3.5: decimal without leading zeros. */
static bool parse_dec(const char *s, uint64_t max, uint64_t *v)
{
    if (s[0] < '0' || s[0] > '9' || (s[0] == '0' && s[1] != '\0')) {
        return false;
    }
    uint64_t n = 0;
    for (const char *p = s; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        uint64_t d = (uint64_t)(*p - '0');
        if (n > (max - d) / 10U) {
            return false;
        }
        n = n * 10U + d;
    }
    *v = n;
    return true;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1; /* §3.4/§3.5: upper case only */
}

static bool parse_hex32(const char *s, uint32_t *v)
{
    uint32_t n = 0;
    for (int i = 0; i < 8; i++) {
        int d = hex_digit(s[i]);
        if (d < 0) {
            return false;
        }
        n = (n << 4U) | (uint32_t)d;
    }
    *v = n;
    return s[8] == '\0';
}

/* §3.4, in place. False if malformed (a raw byte that should have been encoded, a bad %XX). */
/* §3.5: exactly 2 * n upper-case hex digits. */
static bool parse_hex_bytes(const char *s, uint8_t *b, size_t n)
{
    if (strlen(s) != 2 * n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        int hi = hex_digit(s[2 * i]);
        int lo = hex_digit(s[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        b[i] = (uint8_t)((unsigned)hi << 4U | (unsigned)lo);
    }
    return true;
}

static bool decode(char *s)
{
    char *o = s;
    for (const char *p = s; *p != '\0'; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x21U || c > 0x7EU) {
            return false;
        }
        if (c == '%') {
            int hi = hex_digit(p[1]);
            int lo = hi >= 0 ? hex_digit(p[2]) : -1;
            if (lo < 0) {
                return false;
            }
            if (hi == 0 && lo == 0) {
                return false; /* %00 would cut the string short */
            }
            *o++ = (char)(((unsigned)hi << 4U) | (unsigned)lo);
            p += 2;
        } else {
            *o++ = (char)c;
        }
    }
    *o = '\0';
    return true;
}

static bool decode_validator(char *field, char *out)
{
    if (strcmp(field, "-") == 0) {
        out[0] = '\0';
        return true;
    }
    if (!decode(field) || strlen(field) > XC_JOURNAL_MAX_VALIDATOR) {
        return false;
    }
    memcpy(out, field, strlen(field) + 1);
    return true;
}

static int by_path(const void *a, const void *b)
{
    const xc_xdvdfs_entry *const *x = (const xc_xdvdfs_entry *const *)a;
    const xc_xdvdfs_entry *const *y = (const xc_xdvdfs_entry *const *)b;
    return strcmp((*x)->path, (*y)->path);
}

static const xc_xdvdfs_entry *find_path(const xc_xdvdfs_entry **sorted, size_t n, const char *path)
{
    size_t lo = 0;
    size_t hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2U;
        int c = strcmp(sorted[mid]->path, path);
        if (c == 0) {
            return sorted[mid];
        }
        if (c < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return NULL;
}

typedef struct reader {
    char *text;
    size_t len;
    size_t pos;
} reader;

/*
 * The next intact record, NUL-terminated without its CRC; NULL at the end or at
 * a torn line (*torn set). *too_long is set for a complete line over the limit.
 */
static char *next_record(reader *r, bool *torn, bool *too_long)
{
    if (r->pos >= r->len) {
        return NULL;
    }
    char *start = r->text + r->pos;
    char *nl = memchr(start, '\n', r->len - r->pos);
    if (nl == NULL) {
        *torn = true; /* no line ending: the append was cut short */
        return NULL;
    }
    size_t n = (size_t)(nl - start);
    r->pos += n + 1;
    if (n > 0 && start[n - 1] == '\r') {
        n--;
    }
    if (n > XC_JOURNAL_MAX_LINE) {
        *too_long = true;
        return NULL;
    }
    uint32_t want = 0;
    char crc_text[9];
    if (n < 10 || start[n - 10] != ' ' || start[n - 9] != '*') {
        *torn = true;
        return NULL;
    }
    memcpy(crc_text, start + n - 8, 8);
    crc_text[8] = '\0';
    if (!parse_hex32(crc_text, &want) || xc_crc32(0, start, n - 10) != want) {
        *torn = true;
        return NULL;
    }
    start[n - 10] = '\0';
    if (memchr(start, '\0', n - 10) != NULL) {
        *torn = true;
        return NULL;
    }
    return start;
}

/* §6.4: nothing in this folder may be reused. `files` is the listing's file count. */
static xc_err invalid(xc_journal_state *st, size_t files, const char *why)
{
    st->valid = false;
    st->why = why;
    st->done_count = 0;
    st->has_hash = false;
    size_t n = files > 0 ? files : 1;
    memset(st->done, 0, sizeof *st->done * n);
    memset(st->part_bytes, 0, sizeof *st->part_bytes * n);
    return XC_OK;
}

static bool read_identity(reader *r, xc_journal_state *st, bool *torn, bool *too_long,
                          unsigned long *minor)
{
    char *rec = next_record(r, torn, too_long);
    unsigned long major = 0;
    if (rec == NULL || xc_text_check_header(rec, "xcat-journal", XC_JOURNAL_VERSION_MAJOR, &major,
                                            minor) != XC_TEXT_HEADER_OK) {
        return false;
    }
    char *f[6];
    rec = next_record(r, torn, too_long);
    if (rec == NULL || split_fields(rec, f, 6) != 5 || strcmp(f[0], "image") != 0 ||
        !parse_dec(f[1], UINT64_MAX, &st->id.image_size) ||
        !parse_dec(f[2], UINT64_MAX, &st->id.partition_offset) ||
        !parse_hex32(f[3], &st->id.listing_crc) ||
        !parse_dec(f[4], UINT64_MAX, &st->id.file_count)) {
        return false;
    }
    rec = next_record(r, torn, too_long);
    if (rec == NULL || split_fields(rec, f, 6) != 3 || strcmp(f[0], "validators") != 0 ||
        !decode_validator(f[1], st->id.etag) || !decode_validator(f[2], st->id.last_modified)) {
        return false;
    }
    rec = next_record(r, torn, too_long);
    return rec != NULL && split_fields(rec, f, 6) == 2 && strcmp(f[0], "source") == 0 &&
           decode(f[1]);
}

xc_err xc_journal_read(const char *data, size_t len, const xc_xdvdfs_listing *listing,
                       const xc_journal_id *image, xc_journal_state *st)
{
    if (st == NULL || listing == NULL || image == NULL || (data == NULL && len > 0)) {
        return XC_ERR_INVALID_ARG;
    }
    memset(st, 0, sizeof *st);
    size_t files = listing->file_count;
    st->done = calloc(files > 0 ? files : 1, sizeof *st->done);
    st->crc = calloc(files > 0 ? files : 1, sizeof *st->crc);
    st->part_bytes = calloc(files > 0 ? files : 1, sizeof *st->part_bytes);
    st->part_crc = calloc(files > 0 ? files : 1, sizeof *st->part_crc);
    const xc_xdvdfs_entry **sorted =
        (const xc_xdvdfs_entry **)malloc((files > 0 ? files : 1) * sizeof *sorted);
    reader r = {NULL, len, 0};
    if (len <= XC_JOURNAL_MAX_FILE) {
        r.text = malloc(len + 1);
    }
    if (st->done == NULL || st->crc == NULL || st->part_bytes == NULL || st->part_crc == NULL ||
        sorted == NULL || (len <= XC_JOURNAL_MAX_FILE && r.text == NULL)) {
        free((void *)sorted);
        free(r.text);
        xc_journal_state_free(st);
        return XC_ERR_NOMEM;
    }
    xc_err err = XC_OK;
    bool torn = false;
    bool too_long = false;
    unsigned long minor = 0;
    if (len > XC_JOURNAL_MAX_FILE) {
        err = invalid(st, files, "journal larger than 4 MiB");
    } else {
        if (len > 0) {
            memcpy(r.text, data, len);
        }
        r.text[len] = '\0';
        if (!read_identity(&r, st, &torn, &too_long, &minor)) {
            err = invalid(st, files, "journal unreadable (missing or damaged identity)");
        } else if (!xc_journal_same_image(&st->id, image)) {
            err = invalid(st, files, "the folder holds an unfinished install of a different image");
        } else {
            st->valid = true;
        }
    }
    if (st->valid) {
        for (size_t i = 0; i < files; i++) {
            sorted[i] = &listing->files[i];
        }
        qsort((void *)sorted, files, sizeof *sorted, by_path);
        char *rec = NULL;
        while (st->valid && (rec = next_record(&r, &torn, &too_long)) != NULL) {
            char *f[5];
            size_t nf = split_fields(rec, f, 5);
            bool is_done = nf >= 1 && strcmp(f[0], "done") == 0;
            bool is_part = nf >= 1 && strcmp(f[0], "part") == 0;
            if (nf >= 1 && strcmp(f[0], "hash") == 0 && minor >= 2) {
                /* §5 (v1.2): the last one counts. */
                uint64_t off = 0;
                if (nf != 4 || !parse_dec(f[1], UINT64_MAX, &off) || off % XC_HASH_BLOCK != 0 ||
                    off > image->image_size || !parse_hex_bytes(f[2], st->hash_md5, XC_MD5_BYTES) ||
                    !parse_hex_bytes(f[3], st->hash_sha1, XC_SHA1_BYTES)) {
                    err = invalid(st, files, "journal has a malformed record");
                    break;
                }
                st->has_hash = true;
                st->hash_offset = off;
                continue;
            }
            if (nf >= 1 && !is_done && !is_part) {
                if (minor <= XC_JOURNAL_VERSION_MINOR) {
                    err = invalid(st, files, "journal has an unknown record");
                }
                continue; /* a newer minor version's record (§3.3) */
            }
            uint64_t size = 0;
            uint32_t crc = 0;
            if (nf != 4 || !parse_dec(f[1], UINT32_MAX, &size) || !parse_hex32(f[2], &crc) ||
                !decode(f[3])) {
                err = invalid(st, files, "journal has a malformed record");
                break;
            }
            const xc_xdvdfs_entry *e = find_path(sorted, files, f[3]);
            if (e == NULL || (is_done && e->size != size)) {
                err = invalid(st, files, "journal names a file the image doesn't have");
                break;
            }
            size_t idx = (size_t)(e - listing->files);
            if (st->done[idx]) {
                err = invalid(st, files, "journal lists a file twice");
                break;
            }
            if (is_part) {
                /* §5: 0 < bytes < size; the last one counts (a file written again from the
                   start after a failed resume records smaller checkpoints again). */
                if (size == 0 || size >= e->size) {
                    err = invalid(st, files, "journal has a checkpoint outside its file");
                    break;
                }
                st->part_bytes[idx] = (uint32_t)size;
                st->part_crc[idx] = crc;
                continue;
            }
            st->done[idx] = true;
            st->crc[idx] = crc;
            st->part_bytes[idx] = 0;
            st->done_count++;
        }
        if (st->valid && too_long) {
            err = invalid(st, files, "journal has an overlong line");
        }
    }
    st->torn = torn;
    free((void *)sorted);
    free(r.text);
    return err;
}

void xc_journal_state_free(xc_journal_state *st)
{
    if (st != NULL) {
        free(st->done);
        free(st->crc);
        free(st->part_bytes);
        free(st->part_crc);
        st->done = NULL;
        st->crc = NULL;
        st->part_bytes = NULL;
        st->part_crc = NULL;
        st->done_count = 0;
    }
}
