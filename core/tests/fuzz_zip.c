/*
 * libFuzzer target for the zip directory reader (docs/formats/zip.md §2–§6). The input is a
 * whole zip file, read through a memory source.
 *
 * Checked after each call; a violation aborts: a listing has at most XC_ZIP_MAX_ENTRIES
 * entries with non-empty names, every local header offset is below the central directory,
 * which lies inside the input. An opened member is stored or Deflate, unencrypted, its data
 * lies between its local header and the central directory, and a stored member's sizes are
 * equal. Seeds are in core/tests/fuzz-seeds/zip/ (tools/fixtures/zip_seeds.py). Built like
 * fuzz_http.c.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "xcat/zip.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void check(int ok)
{
    if (!ok) {
        abort();
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    xc_mem_source mem;
    xc_source src;
    xc_source_memory(&src, &mem, data, size);
    xc_zip_listing l;
    xc_err err = xc_zip_list(&src, &l);
    if (err != XC_OK) {
        check(l.entries == NULL && l.count == 0);
        return 0;
    }
    check(l.count <= XC_ZIP_MAX_ENTRIES && l.cd_offset <= size);
    for (size_t i = 0; i < l.count; i++) {
        const xc_zip_entry *e = &l.entries[i];
        check(e->name != NULL && e->name[0] != '\0');
        check(e->local_offset < l.cd_offset);
        xc_zip_member m;
        if (xc_zip_open_member(&src, &l, i, &m) == XC_OK) {
            check(m.index == i && m.method == e->method);
            check(m.method == XC_ZIP_STORED || m.method == XC_ZIP_DEFLATE);
            check((e->flags & 0x2041U) == 0);
            check(m.method != XC_ZIP_STORED || m.size == m.compressed_size);
            check(m.data_offset >= e->local_offset + 30U + strlen(e->name));
            check(m.data_offset <= l.cd_offset && m.compressed_size <= l.cd_offset - m.data_offset);
        }
    }
    size_t pick = 0;
    if (xc_zip_pick_image(&l, &pick) == XC_OK) {
        check(pick < l.count && xc_zip_is_image_name(l.entries[pick].name));
    }
    xc_zip_listing_free(&l);
    return 0;
}
