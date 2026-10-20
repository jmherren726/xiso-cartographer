/*
 * libFuzzer target for the catalog (docs/formats/catalog.md).
 *
 * The input is a catalog.bin. The harness writes the right file_size and CRC-32 into the
 * header before opening it, so the fuzzer reaches the structure checks behind them (the
 * CRC check itself is unit tested). An opened catalog must give back, for every release,
 * itself from xc_catalog_find_release and xc_catalog_find_name, and every string must lie
 * inside the input. Errors other than those xc_catalog_open documents, or a violation,
 * abort. Seeds are in core/tests/fuzz-seeds/catalog/ (tools/fixtures/catalog_seeds.py).
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "xcat/catalog.h"
#include "xcat/crc32.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void check(int ok)
{
    if (!ok) {
        abort();
    }
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8U);
    p[2] = (uint8_t)(v >> 16U);
    p[3] = (uint8_t)(v >> 24U);
}

/* The string lies inside [buf, buf + size) with its NUL. */
static void check_string(const uint8_t *buf, size_t size, const char *s)
{
    const uint8_t *p = (const uint8_t *)s;
    check(p >= buf && p < buf + size);
    check(memchr(p, 0, (size_t)(buf + size - p)) != NULL);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > XC_CATALOG_MAX_SIZE) {
        return 0;
    }
    uint8_t *buf = malloc(size > 0 ? size : 1);
    if (buf == NULL) {
        return 0;
    }
    if (size > 0) {
        memcpy(buf, data, size);
    }
    if (size >= 64) {
        put32(buf + 12, (uint32_t)size);
        uint32_t crc = xc_crc32(0, buf, 60);
        put32(buf + 60, xc_crc32(crc, buf + 64, size - 64));
    }
    xc_catalog cat;
    xc_err err = xc_catalog_open(&cat, buf, size);
    check(err == XC_OK || err == XC_ERR_CATALOG || err == XC_ERR_UNSUPPORTED);
    if (err != XC_OK) {
        check(cat.data == NULL && cat.game_count == 0 && cat.release_count == 0);
        free(buf);
        return 0;
    }
    check_string(buf, size, cat.snapshot);
    for (size_t i = 0; i < cat.game_count; i++) {
        xc_catalog_game g;
        check(xc_catalog_game_at(&cat, i, &g));
        check_string(buf, size, g.title);
        check_string(buf, size, g.developer);
        check_string(buf, size, g.publisher);
        check_string(buf, size, g.genres);
        check(g.title[0] != '\0');
        (void)xc_catalog_game_matches(&cat, i, "a");
    }
    for (size_t i = 0; i < cat.release_count; i++) {
        xc_catalog_release r;
        check(xc_catalog_release_at(&cat, i, &r));
        check(r.game < cat.game_count && r.title_id != 0);
        check_string(buf, size, r.variant);
        check_string(buf, size, r.name);
        check_string(buf, size, r.region);
        check(xc_catalog_find_release(&cat, r.title_id, r.variant) == i);
        check(xc_catalog_find_name(&cat, r.name) == i);
        size_t count = 0;
        size_t first = xc_catalog_find_title(&cat, r.title_id, &count);
        check(first <= i && i < first + count);
    }
    xc_catalog_game past;
    check(!xc_catalog_game_at(&cat, cat.game_count, &past));
    free(buf);
    return 0;
}
