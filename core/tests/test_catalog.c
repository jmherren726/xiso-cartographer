/* Tests for the catalog reader (xcat/catalog.h, docs/formats/catalog.md). */
#include "unity.h"
#include "xcat/catalog.h"
#include "xcat/crc32.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* ---- A small catalog writer: a valid file, which tests then break one field at a time. */

typedef struct tgame {
    const char *title;
    const char *developer;
    const char *publisher;
    const char *genres;
    uint32_t wikidata;
    uint16_t year;
} tgame;

typedef struct trel {
    uint32_t game;
    uint32_t title_id;
    const char *variant;
    const char *name;
    const char *region;
    uint32_t redump_id;
    uint64_t size;
} trel;

static uint8_t buf[64 * 1024];
static size_t len;
static size_t games_off;
static size_t releases_off;
static size_t names_off;
static size_t strings_off;
static size_t game_size;
static size_t release_size;

static uint8_t strs[16 * 1024];
static size_t strs_len;

static void put16(size_t off, uint32_t v)
{
    buf[off] = (uint8_t)v;
    buf[off + 1] = (uint8_t)(v >> 8U);
}

static void put32(size_t off, uint32_t v)
{
    put16(off, v & 0xFFFFU);
    put16(off + 2, v >> 16U);
}

static uint32_t get32(size_t off)
{
    return (uint32_t)buf[off] | ((uint32_t)buf[off + 1] << 8U) | ((uint32_t)buf[off + 2] << 16U) |
           ((uint32_t)buf[off + 3] << 24U);
}

/* Each distinct string once; offset 0 is "". */
static uint32_t intern(const char *s)
{
    size_t n = strlen(s);
    if (n == 0) {
        return 0;
    }
    for (size_t off = 1; off + n < strs_len; off++) {
        if (strs[off - 1] == 0 && memcmp(strs + off, s, n + 1) == 0) {
            return (uint32_t)off;
        }
    }
    uint32_t off = (uint32_t)strs_len;
    memcpy(strs + strs_len, s, n + 1);
    strs_len += n + 1;
    return off;
}

static size_t align4(size_t n)
{
    return (n + 3U) & ~(size_t)3U;
}

static void fix_crc(void)
{
    uint32_t crc = xc_crc32(0, buf, 60);
    crc = xc_crc32(crc, buf + 64, len - 64);
    put32(60, crc);
}

/* Writes a catalog of `ng` games and `nr` releases, in the order given; the name index is
 * sorted here. `extra` bytes (zero) lengthen every record, as a newer minor may (§2). */
static void build_ex(const tgame *g, size_t ng, const trel *r, size_t nr, uint16_t minor,
                     size_t extra)
{
    memset(buf, 0, sizeof buf);
    memset(strs, 0, sizeof strs);
    strs_len = 1;
    game_size = 32 + extra;
    release_size = 72 + extra;
    games_off = 64;
    releases_off = align4(games_off + ng * game_size);
    names_off = align4(releases_off + nr * release_size);
    strings_off = align4(names_off + nr * 4);
    uint32_t snapshot = intern("test 2026-10-20");
    for (size_t i = 0; i < ng; i++) {
        size_t o = games_off + i * game_size;
        put32(o, intern(g[i].title));
        put32(o + 4, intern(g[i].developer));
        put32(o + 8, intern(g[i].publisher));
        put32(o + 12, intern(g[i].genres));
        put32(o + 16, g[i].wikidata);
        put16(o + 20, g[i].year);
    }
    for (size_t i = 0; i < nr; i++) {
        size_t o = releases_off + i * release_size;
        put32(o, r[i].game);
        put32(o + 4, r[i].title_id);
        put32(o + 8, intern(r[i].variant));
        put32(o + 12, intern(r[i].name));
        put32(o + 16, intern(r[i].region));
        put32(o + 20, r[i].redump_id);
        put32(o + 24, (uint32_t)r[i].size);
        put32(o + 28, (uint32_t)(r[i].size >> 32U));
        put32(o + 32, 0x11223344U + (uint32_t)i);
        memset(buf + o + 36, (int)(0xA0 + i), 16);
        memset(buf + o + 52, (int)(0xB0 + i), 20);
    }
    /* Name index: insertion sort of the release indices by name. */
    uint32_t idx[64];
    for (size_t i = 0; i < nr; i++) {
        size_t j = i;
        while (j > 0 && xc_catalog_collate(r[idx[j - 1]].name, r[i].name) > 0) {
            idx[j] = idx[j - 1];
            j--;
        }
        idx[j] = (uint32_t)i;
    }
    for (size_t i = 0; i < nr; i++) {
        put32(names_off + 4 * i, idx[i]);
    }
    memcpy(buf + strings_off, strs, strs_len);
    len = strings_off + strs_len;
    memcpy(buf, "XCATCAT", 8);
    put16(8, 1);
    put16(10, minor);
    put32(12, (uint32_t)len);
    put32(16, (uint32_t)ng);
    put32(20, (uint32_t)nr);
    put16(24, (uint32_t)game_size);
    put16(26, (uint32_t)release_size);
    put32(28, (uint32_t)games_off);
    put32(32, (uint32_t)releases_off);
    put32(36, (uint32_t)names_off);
    put32(40, (uint32_t)strings_off);
    put32(44, (uint32_t)strs_len);
    put32(48, snapshot);
    fix_crc();
}

static const tgame games[] = {
    {"Alpha Racer", "Studio One", "Pub A", "racing", 1001, 2002},
    {"bravo Tactics", "", "", "", 0, 0},
    {"Charlie Quest", "Studio Two, Studio Three", "Pub B", "action, adventure", 1003, 2004},
};

/* Sorted by title ID, then variant; names deliberately in another order. */
static const trel releases[] = {
    {0, 0x41410001U, "EU", "Alpha Racer (Europe)", "Europe", 11, 7825162240ULL},
    {0, 0x41410001U, "US", "Alpha Racer (USA)", "USA", 12, 7825162240ULL},
    {2, 0x43430010U, "US-EU", "Charlie Quest (USA, Europe)", "USA, Europe", 31, 8547991552ULL},
    {1, 0x4D530004U, "JP", "Bravo Tactics (Japan)", "Japan", 0, 7307001856ULL},
};

#define NG (sizeof games / sizeof games[0])
#define NR (sizeof releases / sizeof releases[0])

static void build(void)
{
    build_ex(games, NG, releases, NR, 0, 0);
}

static xc_err open_buf(xc_catalog *cat)
{
    return xc_catalog_open(cat, buf, len);
}

/* Re-CRCs the file and expects it to be rejected. */
static void expect_reject(void)
{
    fix_crc();
    xc_catalog cat;
    TEST_ASSERT_EQUAL_INT(XC_ERR_CATALOG, open_buf(&cat));
    TEST_ASSERT_EQUAL_UINT32(0, cat.game_count);
    TEST_ASSERT_NULL(cat.data);
}

static size_t game_field(size_t i, size_t off)
{
    return games_off + i * game_size + off;
}

static size_t release_field(size_t i, size_t off)
{
    return releases_off + i * release_size + off;
}

/* ---- Reading a valid catalog */

static void test_open_and_read(void)
{
    build();
    xc_catalog cat;
    TEST_ASSERT_EQUAL_INT(XC_OK, open_buf(&cat));
    TEST_ASSERT_EQUAL_UINT32(NG, cat.game_count);
    TEST_ASSERT_EQUAL_UINT32(NR, cat.release_count);
    TEST_ASSERT_EQUAL_STRING("test 2026-10-20", cat.snapshot);

    xc_catalog_game g;
    TEST_ASSERT_TRUE(xc_catalog_game_at(&cat, 2, &g));
    TEST_ASSERT_EQUAL_STRING("Charlie Quest", g.title);
    TEST_ASSERT_EQUAL_STRING("Studio Two, Studio Three", g.developer);
    TEST_ASSERT_EQUAL_STRING("Pub B", g.publisher);
    TEST_ASSERT_EQUAL_STRING("action, adventure", g.genres);
    TEST_ASSERT_EQUAL_UINT32(1003, g.wikidata);
    TEST_ASSERT_EQUAL_UINT16(2004, g.year);
    TEST_ASSERT_TRUE(xc_catalog_game_at(&cat, 1, &g));
    TEST_ASSERT_EQUAL_STRING("", g.developer);
    TEST_ASSERT_EQUAL_UINT16(0, g.year);
    TEST_ASSERT_FALSE(xc_catalog_game_at(&cat, NG, &g));

    xc_catalog_release r;
    TEST_ASSERT_TRUE(xc_catalog_release_at(&cat, 2, &r));
    TEST_ASSERT_EQUAL_UINT32(2, r.game);
    TEST_ASSERT_EQUAL_HEX32(0x43430010U, r.title_id);
    TEST_ASSERT_EQUAL_STRING("US-EU", r.variant);
    TEST_ASSERT_EQUAL_STRING("Charlie Quest (USA, Europe)", r.name);
    TEST_ASSERT_EQUAL_STRING("USA, Europe", r.region);
    TEST_ASSERT_EQUAL_UINT32(31, r.redump_id);
    TEST_ASSERT_EQUAL_UINT64(8547991552ULL, r.size);
    TEST_ASSERT_EQUAL_HEX32(0x11223346U, r.crc32);
    TEST_ASSERT_EQUAL_HEX8(0xA2, r.md5[15]);
    TEST_ASSERT_EQUAL_HEX8(0xB2, r.sha1[19]);
    TEST_ASSERT_FALSE(xc_catalog_release_at(&cat, NR, &r));
}

static void test_find(void)
{
    build();
    xc_catalog cat;
    TEST_ASSERT_EQUAL_INT(XC_OK, open_buf(&cat));
    size_t count = 99;
    TEST_ASSERT_EQUAL_size_t(0, xc_catalog_find_title(&cat, 0x41410001U, &count));
    TEST_ASSERT_EQUAL_size_t(2, count);
    TEST_ASSERT_EQUAL_size_t(3, xc_catalog_find_title(&cat, 0x4D530004U, &count));
    TEST_ASSERT_EQUAL_size_t(1, count);
    TEST_ASSERT_EQUAL_size_t(XC_CATALOG_NONE, xc_catalog_find_title(&cat, 0x42420001U, &count));
    TEST_ASSERT_EQUAL_size_t(0, count);
    TEST_ASSERT_EQUAL_size_t(XC_CATALOG_NONE, xc_catalog_find_title(&cat, 0xFFFFFFFFU, &count));
    TEST_ASSERT_EQUAL_size_t(XC_CATALOG_NONE, xc_catalog_find_title(&cat, 1, &count));

    TEST_ASSERT_EQUAL_size_t(1, xc_catalog_find_release(&cat, 0x41410001U, "US"));
    TEST_ASSERT_EQUAL_size_t(0, xc_catalog_find_release(&cat, 0x41410001U, "eu"));
    TEST_ASSERT_EQUAL_size_t(2, xc_catalog_find_release(&cat, 0x43430010U, "us-eu"));
    TEST_ASSERT_EQUAL_size_t(XC_CATALOG_NONE, xc_catalog_find_release(&cat, 0x41410001U, "JP"));
    TEST_ASSERT_EQUAL_size_t(XC_CATALOG_NONE, xc_catalog_find_release(&cat, 0x41410001U, ""));
    TEST_ASSERT_EQUAL_size_t(XC_CATALOG_NONE, xc_catalog_find_release(&cat, 0x42420001U, "US"));

    /* Every release by its name, in any case; near misses aren't found. */
    for (size_t i = 0; i < NR; i++) {
        TEST_ASSERT_EQUAL_size_t(i, xc_catalog_find_name(&cat, releases[i].name));
    }
    TEST_ASSERT_EQUAL_size_t(3, xc_catalog_find_name(&cat, "BRAVO TACTICS (JAPAN)"));
    TEST_ASSERT_EQUAL_size_t(XC_CATALOG_NONE, xc_catalog_find_name(&cat, "Alpha Racer"));
    TEST_ASSERT_EQUAL_size_t(XC_CATALOG_NONE, xc_catalog_find_name(&cat, "Alpha Racer (USA) "));
    TEST_ASSERT_EQUAL_size_t(XC_CATALOG_NONE, xc_catalog_find_name(&cat, ""));
    TEST_ASSERT_EQUAL_size_t(XC_CATALOG_NONE, xc_catalog_find_name(&cat, "Zulu"));

    /* A zeroed catalog (after a failed open) finds nothing. */
    xc_catalog none;
    memset(&none, 0, sizeof none);
    TEST_ASSERT_EQUAL_size_t(XC_CATALOG_NONE, xc_catalog_find_title(&none, 0x41410001U, &count));
    TEST_ASSERT_EQUAL_size_t(XC_CATALOG_NONE, xc_catalog_find_name(&none, "Zulu"));
    TEST_ASSERT_EQUAL_size_t(XC_CATALOG_NONE, xc_catalog_find_release(&none, 0x41410001U, "US"));
    xc_catalog_game g;
    xc_catalog_release r;
    TEST_ASSERT_FALSE(xc_catalog_game_at(&none, 0, &g));
    TEST_ASSERT_FALSE(xc_catalog_release_at(&none, 0, &r));
    TEST_ASSERT_FALSE(xc_catalog_game_matches(&none, 0, ""));
}

static void test_matches(void)
{
    build();
    xc_catalog cat;
    TEST_ASSERT_EQUAL_INT(XC_OK, open_buf(&cat));
    TEST_ASSERT_TRUE(xc_catalog_game_matches(&cat, 0, ""));
    TEST_ASSERT_TRUE(xc_catalog_game_matches(&cat, 0, "RACER"));
    TEST_ASSERT_TRUE(xc_catalog_game_matches(&cat, 0, "studio one"));
    TEST_ASSERT_TRUE(xc_catalog_game_matches(&cat, 2, "pub b"));
    TEST_ASSERT_TRUE(xc_catalog_game_matches(&cat, 2, "Three"));
    TEST_ASSERT_FALSE(xc_catalog_game_matches(&cat, 1, "studio"));
    TEST_ASSERT_FALSE(xc_catalog_game_matches(&cat, 0, "Racers"));
    TEST_ASSERT_FALSE(xc_catalog_game_matches(&cat, NG, ""));
}

static void test_collate(void)
{
    TEST_ASSERT_EQUAL_INT(0, xc_catalog_collate("Halo", "hALO"));
    TEST_ASSERT_TRUE(xc_catalog_collate("Halo", "Halo 2") < 0);
    TEST_ASSERT_TRUE(xc_catalog_collate("b", "A") > 0);
    TEST_ASSERT_TRUE(xc_catalog_collate("Z", "a") > 0);        /* folded: z > a */
    TEST_ASSERT_TRUE(xc_catalog_collate("_", "a") < 0);        /* '_' 0x5F < 'a' 0x61 */
    TEST_ASSERT_TRUE(xc_catalog_collate("_", "A") < 0);        /* 'A' folds to 'a' */
    TEST_ASSERT_TRUE(xc_catalog_collate("e", "\xc3\xa9") < 0); /* nothing else is folded */
    TEST_ASSERT_EQUAL_INT(0, xc_catalog_collate("", ""));
}

/* ---- §2 and §7 steps 1–3: size, magic, version, CRC */

static void test_header(void)
{
    xc_catalog cat;
    build();
    TEST_ASSERT_EQUAL_INT(XC_ERR_CATALOG, xc_catalog_open(&cat, buf, 63));
    TEST_ASSERT_EQUAL_INT(XC_ERR_CATALOG, xc_catalog_open(&cat, buf, 0));
    TEST_ASSERT_EQUAL_INT(XC_ERR_CATALOG, xc_catalog_open(&cat, NULL, 0));
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_catalog_open(NULL, buf, len));
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_catalog_open(&cat, NULL, 64));
    /* Too large is refused before any byte is read. */
    TEST_ASSERT_EQUAL_INT(XC_ERR_LIMIT, xc_catalog_open(&cat, buf, XC_CATALOG_MAX_SIZE + 1));
    /* file_size must be the size given. */
    TEST_ASSERT_EQUAL_INT(XC_ERR_CATALOG, xc_catalog_open(&cat, buf, len - 1));

    build();
    buf[7] = 'X';
    expect_reject();

    build();
    put16(8, 2);
    fix_crc();
    TEST_ASSERT_EQUAL_INT(XC_ERR_UNSUPPORTED, open_buf(&cat));
    build();
    put16(8, 0);
    fix_crc();
    TEST_ASSERT_EQUAL_INT(XC_ERR_UNSUPPORTED, open_buf(&cat));

    /* Any changed byte outside the CRC field, without a new CRC: damaged. */
    build();
    buf[len - 2] ^= 1;
    TEST_ASSERT_EQUAL_INT(XC_ERR_CATALOG, open_buf(&cat));
    build();
    buf[59] ^= 1;
    TEST_ASSERT_EQUAL_INT(XC_ERR_CATALOG, open_buf(&cat));
    build();
    buf[61] ^= 1;
    TEST_ASSERT_EQUAL_INT(XC_ERR_CATALOG, open_buf(&cat));
}

/* ---- §1, §2: counts, record sizes, offsets, padding; minor versions */

static void test_layout(void)
{
    build();
    put32(16, 0); /* no games */
    expect_reject();
    build();
    put32(20, 0);
    expect_reject();
    build();
    put32(16, XC_CATALOG_MAX_COUNT + 1);
    expect_reject();
    build();
    put16(24, 36); /* longer records in our own minor version */
    expect_reject();
    build();
    put16(26, 68);
    expect_reject();
    build();
    put32(28, 68); /* a gap of 4 before the games */
    expect_reject();
    build();
    put32(32, (uint32_t)releases_off + 4);
    expect_reject();
    build();
    put32(44, get32(44) - 1); /* strings end before the file does */
    expect_reject();
    build();
    put32(40, 0xFFFFFFF0U);
    expect_reject();
    build();
    buf[56] = 1; /* reserved */
    expect_reject();
}

static void test_padding(void)
{
    /* 1.0's records and index entries are multiples of 4 bytes, so its parts need no
     * padding. A newer minor that lengthens records by an odd amount does. */
    build_ex(games, NG, releases, NR, 1, 1);
    TEST_ASSERT_TRUE(releases_off > games_off + NG * game_size); /* padded */
    xc_catalog cat;
    TEST_ASSERT_EQUAL_INT(XC_OK, open_buf(&cat));
    buf[games_off + NG * game_size] = 1; /* non-zero padding */
    expect_reject();
}

static void test_newer_minor(void)
{
    xc_catalog cat;
    /* A newer minor: longer records, used reserved bytes and flags are all ignored. */
    build_ex(games, NG, releases, NR, 1, 8);
    buf[56] = 1;
    buf[game_field(0, 22)] = 1;
    buf[game_field(1, 31)] = 1;
    buf[game_field(1, 35)] = 1;    /* in the extra bytes */
    buf[release_field(0, 75)] = 9; /* in the extra bytes */
    fix_crc();
    TEST_ASSERT_EQUAL_INT(XC_OK, open_buf(&cat));
    TEST_ASSERT_EQUAL_UINT16(1, cat.minor);
    xc_catalog_release r;
    TEST_ASSERT_TRUE(xc_catalog_release_at(&cat, 3, &r));
    TEST_ASSERT_EQUAL_STRING("Bravo Tactics (Japan)", r.name);
    xc_catalog_game g;
    TEST_ASSERT_TRUE(xc_catalog_game_at(&cat, 2, &g));
    TEST_ASSERT_EQUAL_STRING("Charlie Quest", g.title);
    TEST_ASSERT_EQUAL_size_t(1, xc_catalog_find_name(&cat, "alpha racer (usa)"));

    /* ...but records shorter than 1.0's are not. */
    build_ex(games, NG, releases, NR, 1, 0);
    put16(24, 28);
    expect_reject();

    /* In 1.0 itself, flags and reserved bytes must be 0. */
    build();
    buf[game_field(0, 22)] = 1;
    expect_reject();
    build();
    buf[game_field(2, 31)] = 1;
    expect_reject();
}

/* ---- §6: strings */

static void test_strings(void)
{
    build();
    size_t s = strings_off + get32(game_field(0, 0)); /* "Alpha Racer" */
    buf[s + 1] = 0x01;
    expect_reject();
    build();
    buf[s + 1] = 0x7F;
    expect_reject();
    build();
    buf[s + 1] = 0xC0; /* overlong lead byte */
    buf[s + 2] = 0x81;
    expect_reject();
    build();
    buf[s + 1] = 0xED; /* surrogate U+D800 */
    buf[s + 2] = 0xA0;
    buf[s + 3] = 0x80;
    expect_reject();
    build();
    buf[strings_off] = 'x'; /* offset 0 isn't "" */
    expect_reject();
    build();
    buf[len - 1] = 'x'; /* the last string isn't ended */
    expect_reject();
    build();
    put32(game_field(0, 0), get32(game_field(0, 0)) + 1); /* inside a string */
    expect_reject();
    build();
    put32(game_field(0, 4), get32(44)); /* past the strings */
    expect_reject();
    build();
    put32(48, get32(44) + 7); /* the snapshot too */
    expect_reject();

    /* 255 bytes are allowed, 256 aren't; multi-byte UTF-8 is fine. */
    char longest[XC_CATALOG_MAX_STRING + 2];
    memset(longest, 'a', sizeof longest);
    longest[XC_CATALOG_MAX_STRING] = '\0';
    tgame g[] = {{longest, "\xc3\xa9\xe2\x82\xac\xf0\x9f\x8e\xae", "", "", 0, 0}};
    trel r[] = {{0, 1, "US", "n", "", 0, 1}};
    build_ex(g, 1, r, 1, 0, 0);
    xc_catalog cat;
    TEST_ASSERT_EQUAL_INT(XC_OK, open_buf(&cat));
    longest[XC_CATALOG_MAX_STRING] = 'a';
    longest[XC_CATALOG_MAX_STRING + 1] = '\0';
    build_ex(g, 1, r, 1, 0, 0);
    expect_reject();
}

/* ---- §3: games */

static void test_games(void)
{
    build();
    put32(game_field(1, 0), 0); /* empty title */
    expect_reject();
    build();
    put16(game_field(0, 20), 1989);
    expect_reject();
    build();
    put16(game_field(0, 20), 2101);
    expect_reject();
    build();
    put16(game_field(0, 20), 1990);
    fix_crc();
    xc_catalog cat;
    TEST_ASSERT_EQUAL_INT(XC_OK, open_buf(&cat));

    /* Out of order: swap two titles. */
    build();
    uint32_t t0 = get32(game_field(0, 0));
    put32(game_field(0, 0), get32(game_field(1, 0)));
    put32(game_field(1, 0), t0);
    expect_reject();

    /* Equal titles (in any case) are ordered by Wikidata number, equal ones allowed. */
    tgame g[] = {
        {"Same", "", "", "", 5, 0}, {"SAME", "", "", "", 7, 0}, {"same", "", "", "", 7, 0}};
    trel r[] = {{0, 1, "A", "a", "", 0, 1}, {1, 1, "B", "b", "", 0, 1}, {2, 1, "C", "c", "", 0, 1}};
    build_ex(g, 3, r, 3, 0, 0);
    TEST_ASSERT_EQUAL_INT(XC_OK, open_buf(&cat));
    g[0].wikidata = 8;
    build_ex(g, 3, r, 3, 0, 0);
    expect_reject();
}

/* ---- §4: releases */

static void test_releases(void)
{
    xc_catalog cat;
    build();
    put32(release_field(0, 0), NG); /* no such game */
    expect_reject();
    build();
    put32(release_field(0, 4), 0); /* title ID 0 */
    expect_reject();
    build();
    put32(release_field(3, 12), 0); /* empty name */
    expect_reject();

    /* Variants: §4.1's characters, upper case, 1–16 of them. */
    static const char *const bad[] = {"", "us", "U S", "US.EU", "ABCDEFGHIJKLMNOPQ", "\xc3\x89"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        trel r[] = {{0, 1, bad[i], "n", "", 0, 1}};
        build_ex(games, 1, r, 1, 0, 0);
        expect_reject();
    }
    trel ok[] = {{0, 1, "ABCDEFGHIJKLMNOP", "n", "", 0, 1}, {0, 1, "Z_9-", "o", "", 0, 1}};
    build_ex(games, 1, ok, 2, 0, 0);
    TEST_ASSERT_EQUAL_INT(XC_OK, open_buf(&cat));

    /* Order: by title ID, then variant; no duplicates. */
    trel unsorted_id[] = {{0, 2, "US", "a", "", 0, 1}, {0, 1, "US", "b", "", 0, 1}};
    build_ex(games, 1, unsorted_id, 2, 0, 0);
    expect_reject();
    trel unsorted_v[] = {{0, 1, "US", "a", "", 0, 1}, {0, 1, "EU", "b", "", 0, 1}};
    build_ex(games, 1, unsorted_v, 2, 0, 0);
    expect_reject();
    trel dup[] = {{0, 1, "US", "a", "", 0, 1}, {0, 1, "US", "b", "", 0, 1}};
    build_ex(games, 1, dup, 2, 0, 0);
    expect_reject();
    trel other_title[] = {{0, 1, "US", "a", "", 0, 1}, {0, 2, "US", "b", "", 0, 1}};
    build_ex(games, 1, other_title, 2, 0, 0);
    TEST_ASSERT_EQUAL_INT(XC_OK, open_buf(&cat));

    /* Every game needs a release. */
    trel only_first[] = {{0, 1, "US", "a", "", 0, 1}};
    build_ex(games, 2, only_first, 1, 0, 0);
    expect_reject();
}

/* ---- §5: name index */

static void test_names(void)
{
    build();
    put32(names_off, get32(names_off + 4)); /* a release twice, one missing */
    expect_reject();
    build();
    put32(names_off, NR); /* no such release */
    expect_reject();
    build();
    uint32_t a = get32(names_off);
    put32(names_off, get32(names_off + 4)); /* swapped: not sorted */
    put32(names_off + 4, a);
    expect_reject();

    /* Names equal under §6.1 are forbidden. */
    trel r[] = {{0, 1, "EU", "Same (Europe)", "", 0, 1}, {0, 1, "US", "SAME (EUROPE)", "", 0, 1}};
    build_ex(games, 1, r, 2, 0, 0);
    expect_reject();
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_open_and_read);
    RUN_TEST(test_find);
    RUN_TEST(test_matches);
    RUN_TEST(test_collate);
    RUN_TEST(test_header);
    RUN_TEST(test_layout);
    RUN_TEST(test_padding);
    RUN_TEST(test_newer_minor);
    RUN_TEST(test_strings);
    RUN_TEST(test_games);
    RUN_TEST(test_releases);
    RUN_TEST(test_names);
    return UNITY_END();
}
