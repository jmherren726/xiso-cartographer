/* The game catalog (xcat/catalog.h, docs/formats/catalog.md). */
#include "xcat/catalog.h"

#include <string.h>

#include "text.h"
#include "xcat/crc32.h"

/*
 * The catalog is checked whole before anything is used (§7), and each part's bounds are
 * checked against the file in layout_ok(). So, unlike streamed formats, the record readers
 * below read fixed offsets inside parts already known to lie within the file, instead of
 * bounds-checking each field (xc_rd_* in bytes.h).
 */

#define HEADER_SIZE 64U
#define CRC_OFF 60U
#define GAME_SIZE 32U    /* §3, version 1.0 */
#define RELEASE_SIZE 72U /* §4, version 1.0 */
#define YEAR_MIN 1990U   /* §3 */
#define YEAR_MAX 2100U

static uint16_t u16(const uint8_t *p)
{
    return (uint16_t)((unsigned)p[0] | ((unsigned)p[1] << 8U));
}

static uint32_t u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) | ((uint32_t)p[2] << 16U) |
           ((uint32_t)p[3] << 24U);
}

static uint64_t u64(const uint8_t *p)
{
    return (uint64_t)u32(p) | ((uint64_t)u32(p + 4) << 32U);
}

static unsigned char fold(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c - 'A' + 'a') : c;
}

int xc_catalog_collate(const char *a, const char *b)
{
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    for (;; x++, y++) {
        unsigned char cx = fold(*x);
        unsigned char cy = fold(*y);
        if (cx != cy || cx == 0) {
            return (int)cx - (int)cy;
        }
    }
}

/* Newer minors may use reserved bytes and flags (§2); ours must keep them 0. */
static bool zero_or_newer(const xc_catalog *cat, const uint8_t *p, size_t n)
{
    if (cat->minor > XC_CATALOG_VERSION_MINOR) {
        return true;
    }
    for (size_t i = 0; i < n; i++) {
        if (p[i] != 0) {
            return false;
        }
    }
    return true;
}

/* §6: every string in the part is valid, bounded UTF-8 with no control characters, and the
 * part starts with the empty string and ends with a NUL. */
static bool strings_ok(const uint8_t *s, size_t n)
{
    if (n == 0 || s[0] != 0 || s[n - 1] != 0) {
        return false;
    }
    size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] != 0) {
            if (s[i] < 0x20U || s[i] == 0x7FU) {
                return false;
            }
            continue;
        }
        size_t len = i - start;
        size_t bad = 0;
        if (len > XC_CATALOG_MAX_STRING || !xc_text_utf8(s + start, len, &bad)) {
            return false;
        }
        start = i + 1;
    }
    return true;
}

/* A string field: inside the part, at the start of a string (§6). */
static bool string_at(const xc_catalog *cat, uint32_t off, const char **out)
{
    const uint8_t *s = cat->data + cat->strings_off;
    if (off >= cat->strings_size || (off > 0 && s[off - 1] != 0)) {
        return false;
    }
    *out = (const char *)(s + off);
    return true;
}

static const uint8_t *game_rec(const xc_catalog *cat, size_t i)
{
    return cat->data + cat->games_off + i * cat->game_size;
}

static const uint8_t *release_rec(const xc_catalog *cat, size_t i)
{
    return cat->data + cat->releases_off + i * cat->release_size;
}

/* §4.1: 1–16 of A–Z 0–9 - _. */
static bool variant_ok(const char *v)
{
    size_t n = strlen(v);
    if (n == 0 || n > XC_CATALOG_MAX_VARIANT) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = v[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) {
            return false;
        }
    }
    return true;
}

/* §1: each part starts at the next multiple of 4 after the previous one, with zero padding. */
static bool part_at(const uint8_t *d, uint64_t prev_end, uint32_t off)
{
    uint64_t want = (prev_end + 3U) & ~(uint64_t)3U;
    if (off != want) {
        return false;
    }
    for (uint64_t i = prev_end; i < want; i++) {
        if (d[i] != 0) {
            return false;
        }
    }
    return true;
}

/* §1–§2: the header, the parts' places and sizes. */
static bool layout_ok(xc_catalog *cat)
{
    const uint8_t *d = cat->data;
    cat->game_count = u32(d + 16);
    cat->release_count = u32(d + 20);
    cat->game_size = u16(d + 24);
    cat->release_size = u16(d + 26);
    cat->games_off = u32(d + 28);
    cat->releases_off = u32(d + 32);
    cat->names_off = u32(d + 36);
    cat->strings_off = u32(d + 40);
    cat->strings_size = u32(d + 44);
    if (cat->game_count == 0 || cat->game_count > XC_CATALOG_MAX_COUNT || cat->release_count == 0 ||
        cat->release_count > XC_CATALOG_MAX_COUNT) {
        return false;
    }
    bool newer = cat->minor > XC_CATALOG_VERSION_MINOR;
    if (newer ? (cat->game_size < GAME_SIZE || cat->release_size < RELEASE_SIZE)
              : (cat->game_size != GAME_SIZE || cat->release_size != RELEASE_SIZE)) {
        return false;
    }
    if (!zero_or_newer(cat, d + 52, 8)) {
        return false;
    }
    /* Counts and sizes are at most 16 bits each, so these sums can't overflow 64 bits. */
    uint64_t games_end = (uint64_t)cat->games_off + (uint64_t)cat->game_count * cat->game_size;
    uint64_t releases_end =
        (uint64_t)cat->releases_off + (uint64_t)cat->release_count * cat->release_size;
    uint64_t names_end = (uint64_t)cat->names_off + (uint64_t)cat->release_count * 4U;
    uint64_t strings_end = (uint64_t)cat->strings_off + cat->strings_size;
    if (cat->strings_size == 0 || strings_end != cat->size || games_end > cat->size ||
        releases_end > cat->size || names_end > cat->size) {
        return false;
    }
    return part_at(d, HEADER_SIZE, cat->games_off) && part_at(d, games_end, cat->releases_off) &&
           part_at(d, releases_end, cat->names_off) && part_at(d, names_end, cat->strings_off);
}

/* §3: every game's strings, year, flags and reserved bytes, and their order. */
static bool games_ok(const xc_catalog *cat)
{
    xc_catalog_game prev = {0};
    for (size_t i = 0; i < cat->game_count; i++) {
        const uint8_t *g = game_rec(cat, i);
        xc_catalog_game cur;
        if (!string_at(cat, u32(g), &cur.title) || !string_at(cat, u32(g + 4), &cur.developer) ||
            !string_at(cat, u32(g + 8), &cur.publisher) ||
            !string_at(cat, u32(g + 12), &cur.genres)) {
            return false;
        }
        cur.wikidata = u32(g + 16);
        cur.year = u16(g + 20);
        if (cur.title[0] == '\0' ||
            (cur.year != 0 && (cur.year < YEAR_MIN || cur.year > YEAR_MAX)) ||
            !zero_or_newer(cat, g + 22, 10)) {
            return false;
        }
        if (i > 0) {
            int c = xc_catalog_collate(prev.title, cur.title);
            if (c > 0 || (c == 0 && prev.wikidata > cur.wikidata)) {
                return false;
            }
        }
        prev = cur;
    }
    return true;
}

static bool bit_test_set(uint8_t *bits, size_t i)
{
    uint8_t m = (uint8_t)(1U << (i & 7U));
    bool was = (bits[i >> 3U] & m) != 0;
    bits[i >> 3U] |= m;
    return was;
}

/* §4: every release's fields and their order; every game has a release. */
static bool releases_ok(const xc_catalog *cat)
{
    uint8_t has_release[(XC_CATALOG_MAX_COUNT + 7U) / 8U] = {0};
    uint32_t prev_id = 0;
    const char *prev_variant = NULL;
    for (size_t i = 0; i < cat->release_count; i++) {
        const uint8_t *r = release_rec(cat, i);
        uint32_t game = u32(r);
        uint32_t title_id = u32(r + 4);
        const char *variant = NULL;
        const char *name = NULL;
        const char *region = NULL;
        if (game >= cat->game_count || title_id == 0 || !string_at(cat, u32(r + 8), &variant) ||
            !string_at(cat, u32(r + 12), &name) || !string_at(cat, u32(r + 16), &region) ||
            !variant_ok(variant) || name[0] == '\0') {
            return false;
        }
        if (prev_variant != NULL &&
            (title_id < prev_id ||
             (title_id == prev_id && xc_catalog_collate(prev_variant, variant) >= 0))) {
            return false;
        }
        (void)bit_test_set(has_release, game);
        prev_id = title_id;
        prev_variant = variant;
    }
    for (size_t g = 0; g < cat->game_count; g++) {
        if ((has_release[g >> 3U] & (1U << (g & 7U))) == 0) {
            return false;
        }
    }
    return true;
}

static const char *release_name(const xc_catalog *cat, uint32_t i)
{
    return (const char *)(cat->data + cat->strings_off + u32(release_rec(cat, i) + 12));
}

/* §5: a permutation of the releases, strictly ascending by name. */
static bool names_ok(const xc_catalog *cat)
{
    uint8_t seen[(XC_CATALOG_MAX_COUNT + 7U) / 8U] = {0};
    const uint8_t *idx = cat->data + cat->names_off;
    const char *prev = NULL;
    for (size_t i = 0; i < cat->release_count; i++) {
        uint32_t r = u32(idx + 4 * i);
        if (r >= cat->release_count || bit_test_set(seen, r)) {
            return false;
        }
        const char *name = release_name(cat, r);
        if (prev != NULL && xc_catalog_collate(prev, name) >= 0) {
            return false;
        }
        prev = name;
    }
    return true;
}

xc_err xc_catalog_open(xc_catalog *cat, const void *data, size_t size)
{
    if (cat == NULL || (data == NULL && size > 0)) {
        return XC_ERR_INVALID_ARG;
    }
    memset(cat, 0, sizeof *cat);
    if (size > XC_CATALOG_MAX_SIZE) {
        return XC_ERR_LIMIT;
    }
    const uint8_t *d = data;
    /* §7 step 1–3: size, magic and version, CRC. */
    if (size < HEADER_SIZE || u32(d + 12) != size || memcmp(d, "XCATCAT\0", 8) != 0) {
        return XC_ERR_CATALOG;
    }
    if (u16(d + 8) != XC_CATALOG_VERSION_MAJOR) {
        return XC_ERR_UNSUPPORTED;
    }
    uint32_t crc = xc_crc32(0, d, CRC_OFF);
    crc = xc_crc32(crc, d + HEADER_SIZE, size - HEADER_SIZE);
    if (crc != u32(d + CRC_OFF)) {
        return XC_ERR_CATALOG;
    }
    /* Steps 4–5. */
    cat->data = d;
    cat->size = size;
    cat->minor = u16(d + 10);
    if (!layout_ok(cat) || !strings_ok(d + cat->strings_off, cat->strings_size) ||
        !string_at(cat, u32(d + 48), &cat->snapshot) || !games_ok(cat) || !releases_ok(cat) ||
        !names_ok(cat)) {
        memset(cat, 0, sizeof *cat);
        return XC_ERR_CATALOG;
    }
    return XC_OK;
}

static const char *str(const xc_catalog *cat, const uint8_t *field)
{
    return (const char *)(cat->data + cat->strings_off + u32(field));
}

bool xc_catalog_game_at(const xc_catalog *cat, size_t i, xc_catalog_game *out)
{
    if (i >= cat->game_count) {
        return false;
    }
    const uint8_t *g = game_rec(cat, i);
    out->title = str(cat, g);
    out->developer = str(cat, g + 4);
    out->publisher = str(cat, g + 8);
    out->genres = str(cat, g + 12);
    out->wikidata = u32(g + 16);
    out->year = u16(g + 20);
    return true;
}

bool xc_catalog_release_at(const xc_catalog *cat, size_t i, xc_catalog_release *out)
{
    if (i >= cat->release_count) {
        return false;
    }
    const uint8_t *r = release_rec(cat, i);
    out->game = u32(r);
    out->title_id = u32(r + 4);
    out->variant = str(cat, r + 8);
    out->name = str(cat, r + 12);
    out->region = str(cat, r + 16);
    out->redump_id = u32(r + 20);
    out->size = u64(r + 24);
    out->crc32 = u32(r + 32);
    memcpy(out->md5, r + 36, sizeof out->md5);
    memcpy(out->sha1, r + 52, sizeof out->sha1);
    return true;
}

/* The first release whose title ID is >= title_id. */
static size_t lower_bound(const xc_catalog *cat, uint32_t title_id)
{
    size_t lo = 0;
    size_t hi = cat->release_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (u32(release_rec(cat, mid) + 4) < title_id) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

size_t xc_catalog_find_title(const xc_catalog *cat, uint32_t title_id, size_t *count)
{
    size_t first = lower_bound(cat, title_id);
    size_t end = first;
    while (end < cat->release_count && u32(release_rec(cat, end) + 4) == title_id) {
        end++;
    }
    *count = end - first;
    return end > first ? first : XC_CATALOG_NONE;
}

size_t xc_catalog_find_release(const xc_catalog *cat, uint32_t title_id, const char *variant)
{
    size_t count = 0;
    size_t first = xc_catalog_find_title(cat, title_id, &count);
    for (size_t i = 0; i < count; i++) {
        if (xc_catalog_collate(str(cat, release_rec(cat, first + i) + 8), variant) == 0) {
            return first + i;
        }
    }
    return XC_CATALOG_NONE;
}

size_t xc_catalog_find_name(const xc_catalog *cat, const char *name)
{
    if (cat->data == NULL) { /* zeroed by a failed open: even data + 0 is undefined */
        return XC_CATALOG_NONE;
    }
    const uint8_t *idx = cat->data + cat->names_off;
    size_t lo = 0;
    size_t hi = cat->release_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        uint32_t r = u32(idx + 4 * mid);
        int c = xc_catalog_collate(release_name(cat, r), name);
        if (c == 0) {
            return r;
        }
        if (c < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return XC_CATALOG_NONE;
}

/* ASCII case-insensitive substring search. */
static bool contains_ci(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; *hay != '\0'; hay++) {
        size_t i = 0;
        while (i < n && hay[i] != '\0' &&
               fold((unsigned char)hay[i]) == fold((unsigned char)needle[i])) {
            i++;
        }
        if (i == n) {
            return true;
        }
    }
    return n == 0;
}

bool xc_catalog_game_matches(const xc_catalog *cat, size_t i, const char *query)
{
    xc_catalog_game g;
    if (!xc_catalog_game_at(cat, i, &g)) {
        return false;
    }
    return contains_ci(g.title, query) || contains_ci(g.developer, query) ||
           contains_ci(g.publisher, query);
}
