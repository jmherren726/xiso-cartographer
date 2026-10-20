/*
 * libFuzzer target for archive.org item listings (docs/formats/archiveorg.md).
 *
 * The input is a metadata response body. After parsing, the result is checked against what
 * the map loader relies on; a violation aborts:
 *
 *   - the status is one of the four, and only XC_ARCHIVEORG_OK has files;
 *   - every file has a valid path (§3.1), a non-empty name that is its last segment without
 *     an image or zip extension (§4), a size from 1 to 2^63 - 1, and a URL that the HTTP
 *     client accepts and that starts with the item's download prefix;
 *   - the counts and the first invalid name stay within their limits.
 *
 * Seeds are in core/tests/fuzz-seeds/archiveorg/. Built like fuzz_http.c.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "xcat/archiveorg.h"
#include "xcat/url.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#define PREFIX "https://archive.org/download/fuzz-item/"

static void check(int ok)
{
    if (!ok) {
        abort();
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    xc_archiveorg_item item;
    xc_err err = xc_archiveorg_parse("fuzz-item", (const char *)data, size, &item);
    check(err == XC_OK || err == XC_ERR_LIMIT);
    check(item.status <= XC_ARCHIVEORG_MISSING);
    check(item.status == XC_ARCHIVEORG_OK || item.file_count == 0);
    check(item.file_count <= XC_ARCHIVEORG_MAX_FILES);
    check(item.invalid > 0 || item.first_invalid[0] == '\0');
    check(strlen(item.first_invalid) <= XC_ARCHIVEORG_MAX_NAME);
    static xc_url u;
    for (size_t i = 0; i < item.file_count; i++) {
        const xc_archiveorg_file *f = &item.files[i];
        size_t plen = strlen(f->path);
        size_t nlen = strlen(f->name);
        check(plen >= 1 && plen <= XC_ARCHIVEORG_MAX_NAME);
        check(f->path[0] != '/' && f->path[plen - 1] != '/' && strstr(f->path, "//") == NULL);
        check(strchr(f->path, '\\') == NULL);
        for (size_t k = 0; k < plen; k++) {
            unsigned char c = (unsigned char)f->path[k];
            check(c >= 0x20U && c != 0x7FU);
        }
        const char *base = strrchr(f->path, '/');
        base = base != NULL ? base + 1 : f->path;
        check(nlen >= 1 && nlen < strlen(base) && memcmp(base, f->name, nlen) == 0);
        check(base[nlen] == '.');
        check(f->size >= 1 && f->size <= UINT64_C(0x7FFFFFFFFFFFFFFF));
        check(strncmp(f->url, PREFIX, strlen(PREFIX)) == 0);
        check(strchr(f->url, ' ') == NULL && strchr(f->url, '#') == NULL);
        check(xc_url_parse(f->url, strlen(f->url), &u) == XC_OK);
    }
    xc_archiveorg_free(&item);
    return 0;
}
