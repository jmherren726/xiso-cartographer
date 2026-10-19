/*
 * Zip central directory reader: docs/formats/zip.md §2–§6.
 */
#include "xcat/zip.h"

#include <stdlib.h>
#include <string.h>

#include "bytes.h"

#define SIG_EOCD 0x06054b50U
#define SIG_Z64_LOCATOR 0x07064b50U
#define SIG_Z64_EOCD 0x06064b50U
#define SIG_CENTRAL 0x02014b50U
#define SIG_LOCAL 0x04034b50U

#define EOCD_LEN 22U
#define LOCATOR_LEN 20U
#define Z64_EOCD_LEN 56U
#define CENTRAL_LEN 46U
#define LOCAL_LEN 30U
#define MAX_COMMENT 65535U

#define ZIP64_EXTRA_ID 0x0001U

#define FLAG_ENCRYPTED 0x0001U
#define FLAG_STRONG 0x0040U
#define FLAG_MASKED_CD 0x2000U

/* The end of the archive as zip.md §2–§3 finds it. */
typedef struct zip_end {
    uint64_t entries;
    uint64_t cd_size;
    uint64_t cd_offset;
    uint64_t area_end; /* where the Zip64 end record or the EOCD starts */
    bool zip64;
} zip_end;

/* §3: the Zip64 locator at `loc` (absolute) and the end record it points to. */
static xc_err read_zip64(const xc_source *src, const uint8_t *locator, uint64_t loc, zip_end *e)
{
    uint32_t disk = 0;
    uint64_t rec = 0;
    uint32_t disks = 0;
    (void)xc_rd_u32le(locator, LOCATOR_LEN, 4, &disk);
    (void)xc_rd_u64le(locator, LOCATOR_LEN, 8, &rec);
    (void)xc_rd_u32le(locator, LOCATOR_LEN, 16, &disks);
    if (disk != 0 || disks != 1) {
        return XC_ERR_ZIP_UNSUPPORTED;
    }
    if (rec > loc || loc - rec < Z64_EOCD_LEN) {
        return XC_ERR_CORRUPT;
    }
    uint8_t r[Z64_EOCD_LEN];
    xc_err err = xc_source_read(src, rec, r, sizeof r);
    if (err != XC_OK) {
        return err;
    }
    uint32_t sig = 0;
    uint64_t rest = 0;
    uint32_t this_disk = 0;
    uint32_t cd_disk = 0;
    uint64_t on_disk = 0;
    (void)xc_rd_u32le(r, sizeof r, 0, &sig);
    (void)xc_rd_u64le(r, sizeof r, 4, &rest);
    (void)xc_rd_u32le(r, sizeof r, 16, &this_disk);
    (void)xc_rd_u32le(r, sizeof r, 20, &cd_disk);
    (void)xc_rd_u64le(r, sizeof r, 24, &on_disk);
    (void)xc_rd_u64le(r, sizeof r, 32, &e->entries);
    (void)xc_rd_u64le(r, sizeof r, 40, &e->cd_size);
    (void)xc_rd_u64le(r, sizeof r, 48, &e->cd_offset);
    if (sig != SIG_Z64_EOCD || rest < Z64_EOCD_LEN - 12U || rest > loc - rec - 12U) {
        return XC_ERR_CORRUPT;
    }
    if (this_disk != 0 || cd_disk != 0 || on_disk != e->entries) {
        return XC_ERR_ZIP_UNSUPPORTED;
    }
    e->area_end = rec;
    e->zip64 = true;
    return XC_OK;
}

/* §2–§3: finds the EOCD in the file's tail and, if there is one, the Zip64 end record. */
static xc_err find_end(const xc_source *src, zip_end *e)
{
    uint64_t size = src->size;
    if (size < EOCD_LEN) {
        return XC_ERR_NOT_ZIP;
    }
    size_t tail_len = LOCATOR_LEN + EOCD_LEN + MAX_COMMENT;
    if (size < tail_len) {
        tail_len = (size_t)size;
    }
    uint64_t tail_at = size - tail_len;
    uint8_t *tail = malloc(tail_len);
    if (tail == NULL) {
        return XC_ERR_NOMEM;
    }
    xc_err err = xc_source_read(src, tail_at, tail, tail_len);
    if (err != XC_OK) {
        free(tail);
        return err;
    }

    size_t pos = tail_len - EOCD_LEN;
    bool found = false;
    for (;;) {
        uint32_t sig = 0;
        uint16_t k = 0;
        (void)xc_rd_u32le(tail, tail_len, pos, &sig);
        (void)xc_rd_u16le(tail, tail_len, pos + 20, &k);
        if (sig == SIG_EOCD && pos + EOCD_LEN + k == tail_len) {
            found = true;
            break;
        }
        if (pos == 0) {
            break;
        }
        pos--;
    }
    if (!found) {
        free(tail);
        return XC_ERR_NOT_ZIP;
    }

    uint16_t disk = 0;
    uint16_t cd_disk = 0;
    uint16_t on_disk = 0;
    uint16_t total = 0;
    uint32_t cd_size = 0;
    uint32_t cd_offset = 0;
    (void)xc_rd_u16le(tail, tail_len, pos + 4, &disk);
    (void)xc_rd_u16le(tail, tail_len, pos + 6, &cd_disk);
    (void)xc_rd_u16le(tail, tail_len, pos + 8, &on_disk);
    (void)xc_rd_u16le(tail, tail_len, pos + 10, &total);
    (void)xc_rd_u32le(tail, tail_len, pos + 12, &cd_size);
    (void)xc_rd_u32le(tail, tail_len, pos + 16, &cd_offset);
    if (disk != 0 || cd_disk != 0 || on_disk != total) {
        free(tail);
        return XC_ERR_ZIP_UNSUPPORTED;
    }

    memset(e, 0, sizeof *e);
    uint32_t loc_sig = 0;
    if (pos >= LOCATOR_LEN && xc_rd_u32le(tail, tail_len, pos - LOCATOR_LEN, &loc_sig) &&
        loc_sig == SIG_Z64_LOCATOR) {
        err = read_zip64(src, tail + pos - LOCATOR_LEN, tail_at + pos - LOCATOR_LEN, e);
        free(tail);
        return err;
    }
    free(tail);
    if (total == 0xFFFFU || cd_size == 0xFFFFFFFFU || cd_offset == 0xFFFFFFFFU) {
        return XC_ERR_CORRUPT; /* §3.2: a Zip64 record is missing */
    }
    e->entries = total;
    e->cd_size = cd_size;
    e->cd_offset = cd_offset;
    e->area_end = tail_at + pos;
    return XC_OK;
}

/* §4.6–§4.7: walks the extra fields; replaces all-ones header values from the Zip64 field. */
static xc_err apply_extra(const uint8_t *x, size_t m, uint32_t size32, uint32_t comp32,
                          uint32_t off32, uint16_t disk16, xc_zip_entry *en, uint32_t *disk)
{
    size_t need = (size32 == 0xFFFFFFFFU ? 8U : 0U) + (comp32 == 0xFFFFFFFFU ? 8U : 0U) +
                  (off32 == 0xFFFFFFFFU ? 8U : 0U) + (disk16 == 0xFFFFU ? 4U : 0U);
    bool replaced = false;
    size_t q = 0;
    while (q < m) {
        uint16_t id = 0;
        uint16_t len = 0;
        if (!xc_rd_u16le(x, m, q, &id) || !xc_rd_u16le(x, m, q + 2, &len) ||
            !xc_in_bounds(m, q + 4, len)) {
            return XC_ERR_CORRUPT;
        }
        if (id == ZIP64_EXTRA_ID && !replaced && need > 0) {
            if (len < need) {
                return XC_ERR_CORRUPT;
            }
            const uint8_t *d = x + q + 4;
            size_t at = 0;
            if (size32 == 0xFFFFFFFFU) {
                (void)xc_rd_u64le(d, len, at, &en->size);
                at += 8;
            }
            if (comp32 == 0xFFFFFFFFU) {
                (void)xc_rd_u64le(d, len, at, &en->compressed_size);
                at += 8;
            }
            if (off32 == 0xFFFFFFFFU) {
                (void)xc_rd_u64le(d, len, at, &en->local_offset);
                at += 8;
            }
            if (disk16 == 0xFFFFU) {
                (void)xc_rd_u32le(d, len, at, disk);
            }
            replaced = true;
        }
        q += 4U + len;
    }
    return need > 0 && !replaced ? XC_ERR_CORRUPT : XC_OK;
}

/* §4.3–§4.9: one central directory header at cd[*pos]; advances *pos past it. */
static xc_err parse_central(const uint8_t *cd, size_t cd_len, size_t *pos, uint64_t cd_offset,
                            xc_zip_entry *en)
{
    size_t p = *pos;
    uint32_t sig = 0;
    if (!xc_in_bounds(cd_len, p, CENTRAL_LEN) || !xc_rd_u32le(cd, cd_len, p, &sig) ||
        sig != SIG_CENTRAL) {
        return XC_ERR_CORRUPT;
    }
    uint32_t comp32 = 0;
    uint32_t size32 = 0;
    uint16_t n = 0;
    uint16_t m = 0;
    uint16_t k = 0;
    uint16_t disk16 = 0;
    uint32_t off32 = 0;
    (void)xc_rd_u16le(cd, cd_len, p + 8, &en->flags);
    (void)xc_rd_u16le(cd, cd_len, p + 10, &en->method);
    (void)xc_rd_u32le(cd, cd_len, p + 16, &en->crc32);
    (void)xc_rd_u32le(cd, cd_len, p + 20, &comp32);
    (void)xc_rd_u32le(cd, cd_len, p + 24, &size32);
    (void)xc_rd_u16le(cd, cd_len, p + 28, &n);
    (void)xc_rd_u16le(cd, cd_len, p + 30, &m);
    (void)xc_rd_u16le(cd, cd_len, p + 32, &k);
    (void)xc_rd_u16le(cd, cd_len, p + 34, &disk16);
    (void)xc_rd_u32le(cd, cd_len, p + 42, &off32);
    size_t var = (size_t)n + m + k;
    if (!xc_in_bounds(cd_len, p + CENTRAL_LEN, var)) {
        return XC_ERR_CORRUPT;
    }
    const uint8_t *name = cd + p + CENTRAL_LEN;
    if (n == 0 || memchr(name, 0, n) != NULL) {
        return XC_ERR_CORRUPT;
    }
    en->compressed_size = comp32;
    en->size = size32;
    en->local_offset = off32;
    uint32_t disk = disk16;
    xc_err err = apply_extra(name + n, m, size32, comp32, off32, disk16, en, &disk);
    if (err != XC_OK) {
        return err;
    }
    if (disk != 0) {
        return XC_ERR_ZIP_UNSUPPORTED;
    }
    if (en->local_offset >= cd_offset) {
        return XC_ERR_CORRUPT;
    }
    en->name = malloc((size_t)n + 1U);
    if (en->name == NULL) {
        return XC_ERR_NOMEM;
    }
    memcpy(en->name, name, n);
    en->name[n] = '\0';
    *pos = p + CENTRAL_LEN + var;
    return XC_OK;
}

void xc_zip_listing_free(xc_zip_listing *listing)
{
    if (listing == NULL) {
        return;
    }
    for (size_t i = 0; i < listing->count; i++) {
        free(listing->entries[i].name);
    }
    free(listing->entries);
    memset(listing, 0, sizeof *listing);
}

xc_err xc_zip_list(const xc_source *src, xc_zip_listing *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof *out);
    }
    if (src == NULL || out == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    zip_end e;
    xc_err err = find_end(src, &e);
    if (err != XC_OK) {
        return err;
    }
    /* §4.1–§4.2 */
    if (e.cd_offset > e.area_end || e.cd_size > e.area_end - e.cd_offset) {
        return XC_ERR_CORRUPT;
    }
    if (e.cd_size > (uint64_t)XC_ZIP_MAX_CD_SIZE || e.entries > XC_ZIP_MAX_ENTRIES) {
        return XC_ERR_LIMIT;
    }
    if (e.entries * CENTRAL_LEN > e.cd_size) {
        return XC_ERR_CORRUPT;
    }
    size_t cd_len = (size_t)e.cd_size;
    size_t count = (size_t)e.entries;
    uint8_t *cd = malloc(cd_len > 0 ? cd_len : 1U);
    xc_zip_entry *entries = calloc(count > 0 ? count : 1U, sizeof *entries);
    if (cd == NULL || entries == NULL) {
        free(cd);
        free(entries);
        return XC_ERR_NOMEM;
    }
    err = cd_len > 0 ? xc_source_read(src, e.cd_offset, cd, cd_len) : XC_OK;
    out->entries = entries;
    size_t pos = 0;
    for (size_t i = 0; err == XC_OK && i < count; i++) {
        err = parse_central(cd, cd_len, &pos, e.cd_offset, &entries[i]);
        if (entries[i].name != NULL) {
            out->count = i + 1;
        }
    }
    free(cd);
    if (err != XC_OK) {
        xc_zip_listing_free(out);
        return err;
    }
    out->cd_offset = e.cd_offset;
    out->zip64 = e.zip64;
    return XC_OK;
}

static bool ends_with_ci(const char *s, size_t len, const char *suffix)
{
    size_t n = strlen(suffix);
    if (len < n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = s[len - n + i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != suffix[i]) {
            return false;
        }
    }
    return true;
}

bool xc_zip_is_image_name(const char *name)
{
    if (name == NULL) {
        return false;
    }
    size_t len = strlen(name);
    return ends_with_ci(name, len, ".iso") || ends_with_ci(name, len, ".xiso");
}

xc_err xc_zip_pick_image(const xc_zip_listing *listing, size_t *index)
{
    if (listing == NULL || index == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    size_t found = 0;
    for (size_t i = 0; i < listing->count; i++) {
        if (xc_zip_is_image_name(listing->entries[i].name)) {
            *index = i;
            found++;
        }
    }
    return found == 1 ? XC_OK : XC_ERR_ZIP_MEMBER;
}

const char *xc_zip_method_name(uint16_t method)
{
    switch (method) {
    case 0:
        return "stored";
    case 8:
        return "Deflate";
    case 9:
        return "Deflate64";
    case 12:
        return "bzip2";
    case 14:
        return "LZMA";
    case 93:
        return "Zstandard";
    case 95:
        return "xz";
    case 98:
        return "PPMd";
    case 99:
        return "AES-encrypted";
    default:
        return NULL;
    }
}

xc_err xc_zip_open_member(const xc_source *src, const xc_zip_listing *listing, size_t index,
                          xc_zip_member *out)
{
    if (src == NULL || listing == NULL || out == NULL || index >= listing->count) {
        return XC_ERR_INVALID_ARG;
    }
    const xc_zip_entry *en = &listing->entries[index];
    /* §6.1–§6.3 */
    if ((en->flags & (FLAG_ENCRYPTED | FLAG_STRONG | FLAG_MASKED_CD)) != 0 ||
        (en->method != XC_ZIP_STORED && en->method != XC_ZIP_DEFLATE)) {
        return XC_ERR_ZIP_UNSUPPORTED;
    }
    if (en->method == XC_ZIP_STORED && en->compressed_size != en->size) {
        return XC_ERR_CORRUPT;
    }
    /* §6.4: the local header */
    uint64_t lo = en->local_offset;
    uint64_t end = listing->cd_offset;
    if (lo > end || end - lo < LOCAL_LEN) {
        return XC_ERR_CORRUPT;
    }
    uint8_t h[LOCAL_LEN];
    xc_err err = xc_source_read(src, lo, h, sizeof h);
    if (err != XC_OK) {
        return err;
    }
    uint32_t sig = 0;
    uint16_t method = 0;
    uint16_t n = 0;
    uint16_t m = 0;
    (void)xc_rd_u32le(h, sizeof h, 0, &sig);
    (void)xc_rd_u16le(h, sizeof h, 8, &method);
    (void)xc_rd_u16le(h, sizeof h, 26, &n);
    (void)xc_rd_u16le(h, sizeof h, 28, &m);
    size_t name_len = strlen(en->name);
    if (sig != SIG_LOCAL || method != en->method || n != name_len ||
        end - lo - LOCAL_LEN < (uint64_t)n + m) {
        return XC_ERR_CORRUPT;
    }
    uint8_t chunk[256];
    for (size_t done = 0; done < name_len;) {
        size_t len = name_len - done < sizeof chunk ? name_len - done : sizeof chunk;
        err = xc_source_read(src, lo + LOCAL_LEN + done, chunk, len);
        if (err != XC_OK) {
            return err;
        }
        if (memcmp(chunk, en->name + done, len) != 0) {
            return XC_ERR_CORRUPT;
        }
        done += len;
    }
    uint64_t data = lo + LOCAL_LEN + n + m;
    if (en->compressed_size > end - data) {
        return XC_ERR_CORRUPT;
    }
    out->index = index;
    out->method = en->method;
    out->crc32 = en->crc32;
    out->compressed_size = en->compressed_size;
    out->size = en->size;
    out->data_offset = data;
    return XC_OK;
}
