/*
 * libFuzzer target for the map loader (docs/formats/map.md).
 *
 * The input is split at NUL bytes into files: part 0 is the top-level map
 * ("maps/top.txt"); part N is served for any @include whose resolved location
 * ends in "fN.txt" (so includes, nesting, cycles and the file limit are all
 * reachable). After loading, the result is checked against the guarantees the
 * installer relies on; a violation aborts:
 *
 *   - limits hold (entries, files, stored problems);
 *   - every entry has a valid key, a URL the HTTP client accepts, and a source;
 *   - keys are unique (§4.3) and xc_map_find returns each entry for its key;
 *   - every problem names a loaded source.
 *
 * Seeds are in core/tests/fuzz-seeds/map/. Built like fuzz_http.c.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "xcat/map.h"
#include "xcat/url.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#define MAX_PARTS 40

static const uint8_t *part_data[MAX_PARTS];
static size_t part_len[MAX_PARTS];
static size_t parts;

static void check(int ok)
{
    if (!ok) {
        abort();
    }
}

static xc_err fetch(void *ctx, const char *location, bool is_url, size_t max_len, char **out,
                    size_t *len)
{
    (void)ctx;
    (void)is_url;
    size_t n = strlen(location);
    if (n < 6 || strcmp(location + n - 4, ".txt") != 0) {
        return XC_ERR_IO;
    }
    /* "...f<digits>.txt" */
    size_t end = n - 4;
    size_t start = end;
    while (start > 0 && location[start - 1] >= '0' && location[start - 1] <= '9') {
        start--;
    }
    if (start == end || start == 0 || location[start - 1] != 'f' || end - start > 2) {
        return XC_ERR_IO;
    }
    size_t index = (size_t)strtoul(location + start, NULL, 10);
    if (index == 0 || index >= parts) {
        return XC_ERR_IO;
    }
    if (part_len[index] > max_len) {
        return XC_ERR_LIMIT;
    }
    *out = malloc(part_len[index] + 1);
    if (*out == NULL) {
        return XC_ERR_NOMEM;
    }
    memcpy(*out, part_data[index], part_len[index]);
    *len = part_len[index];
    return XC_OK;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    parts = 0;
    size_t start = 0;
    for (size_t i = 0; i <= size && parts < MAX_PARTS; i++) {
        if (i == size || data[i] == 0) {
            part_data[parts] = data + start;
            part_len[parts] = i - start;
            parts++;
            start = i + 1;
        }
    }
    xc_map map;
    xc_map_loader loader = {fetch, NULL};
    xc_err err =
        xc_map_load((const char *)part_data[0], part_len[0], "maps/top.txt", &loader, &map);
    check(err == XC_OK || err == XC_ERR_MAP);
    check(map.entry_count <= XC_MAP_MAX_ENTRIES);
    check(map.source_count >= 1 && map.source_count <= XC_MAP_MAX_FILES);
    check(map.problem_count <= XC_MAP_MAX_PROBLEMS);
    check(err == XC_OK || map.entry_count == 0);
    size_t stored_errors = 0;
    for (size_t i = 0; i < map.problem_count; i++) {
        const xc_map_problem *p = &map.problems[i];
        check(p->source < map.source_count);
        check(memchr(p->message, '\0', sizeof p->message) != NULL);
        stored_errors += p->severity == XC_MAP_ERROR ? 1U : 0U;
    }
    check(stored_errors <= map.error_count);
    static xc_url u;
    for (size_t i = 0; i < map.entry_count; i++) {
        const xc_map_entry *e = &map.entries[i];
        check(e->source < map.source_count && e->line >= 1);
        check(strlen(e->variant) <= XC_MAP_MAX_VARIANT);
        for (const char *c = e->variant; *c != '\0'; c++) {
            check(!(*c >= 'a' && *c <= 'z'));
        }
        check(xc_url_parse(e->url, strlen(e->url), &u) == XC_OK);
        check(xc_map_find(&map, e->title_id, e->variant) == e);
        for (size_t k = 0; k < i; k++) {
            check(map.entries[k].title_id != e->title_id ||
                  strcmp(map.entries[k].variant, e->variant) != 0);
        }
    }
    xc_map_free(&map);
    return 0;
}
