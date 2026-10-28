/*
 * libFuzzer target for editing a map's sources (xcat/map_edit.h, docs/formats/map.md §9).
 *
 * The first byte picks what the rest is: 'm' a map whose source lines are listed, then each
 * removed in turn and one added; 'e', 'i' or 'a' what's typed for a new entry's URL (with the
 * key 4D530004), an included map's address or an archive.org identifier. Listing gives XC_OK,
 * XC_ERR_MAP or XC_ERR_LIMIT, with every line inside the data; removing a line leaves one
 * fewer and adding one more, with every other byte kept; a new line that checks out loads, in
 * a map of its own, without errors. A violation aborts. Seeds are in
 * core/tests/fuzz-seeds/map-edit/.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "xcat/map.h"
#include "xcat/map_edit.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#define MAX_LINES 64

static void check(int ok)
{
    if (!ok) {
        abort();
    }
}

static size_t count_lines(const char *data, size_t len)
{
    size_t count = 0;
    check(xc_map_lines(data, len, NULL, 0, &count) == XC_OK);
    return count;
}

static void edit(const char *data, size_t len)
{
    static xc_map_line lines[MAX_LINES];
    size_t count = 0;
    if (data == NULL) { /* an empty file: no header */
        check(xc_map_lines(NULL, 0, lines, MAX_LINES, &count) == XC_ERR_MAP && count == 0);
        return;
    }
    xc_err err = xc_map_lines(data, len, lines, MAX_LINES, &count);
    check(err == XC_OK || err == XC_ERR_MAP || err == XC_ERR_LIMIT);
    if (err != XC_OK) {
        check(count == 0);
        return;
    }
    size_t prev = 0;
    for (size_t i = 0; i < count && i < MAX_LINES; i++) {
        const xc_map_line *l = &lines[i];
        check(l->line > prev);
        prev = l->line;
        check(l->text >= data && l->text + l->text_len <= data + len && l->text_len > 0);
        check(l->arg >= l->text && l->arg + l->arg_len <= l->text + l->text_len);
        check(l->key_len <= l->text_len);
        check(l->text[0] != '#');
        char *out = NULL;
        size_t out_len = 0;
        check(xc_map_remove_line(data, len, l->line, &out, &out_len) == XC_OK);
        check(out != NULL && out[out_len] == '\0' && out_len < len);
        check(count_lines(out, out_len) == count - 1);
        free(out);
    }
    char *out = NULL;
    size_t out_len = 0;
    err = xc_map_add_line(data, len, "@archiveorg fuzz", &out, &out_len);
    check(err == XC_OK || err == XC_ERR_LIMIT);
    if (err == XC_OK) {
        check(out_len > len && memcmp(out, data, len) == 0);
        check(count_lines(out, out_len) == count + 1);
        free(out);
    }
}

static void typed(char kind, const char *data, size_t len)
{
    char text[XC_MAP_MAX_LINE + 1];
    if (len >= sizeof text || memchr(data, '\0', len) != NULL) {
        return;
    }
    memcpy(text, data, len);
    text[len] = '\0';
    xc_map_line_kind k = kind == 'e'   ? XC_MAP_LINE_ENTRY
                         : kind == 'i' ? XC_MAP_LINE_INCLUDE
                                       : XC_MAP_LINE_ARCHIVEORG;
    char line[XC_MAP_MAX_LINE + 64];
    char why[200];
    xc_err err = xc_map_new_line(k, "4D530004", text, line, sizeof line, why, sizeof why);
    check(err == XC_OK || err == XC_ERR_MAP || err == XC_ERR_LIMIT);
    check(memchr(why, '\0', sizeof why) != NULL);
    if (err != XC_OK) {
        check(line[0] == '\0');
        return;
    }
    char *out = NULL;
    size_t out_len = 0;
    check(xc_map_add_line(NULL, 0, line, &out, &out_len) == XC_OK);
    check(count_lines(out, out_len) == 1);
    if (k == XC_MAP_LINE_ENTRY) {
        xc_map map;
        check(xc_map_load(out, out_len, "map.txt", NULL, &map) == XC_OK);
        check(map.error_count == 0 && map.entry_count == 1);
        xc_map_free(&map);
    }
    free(out);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0) {
        return 0;
    }
    const char *rest = (const char *)data + 1;
    switch (data[0]) {
    case 'm':
        edit(size > 1 ? rest : NULL, size - 1);
        break;
    case 'e':
    case 'i':
    case 'a':
        typed((char)data[0], rest, size - 1);
        break;
    default:
        break;
    }
    return 0;
}
