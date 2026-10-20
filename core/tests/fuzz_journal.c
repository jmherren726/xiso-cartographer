/*
 * libFuzzer target for the install journal reader (docs/formats/journal.md).
 *
 * Every input is read twice against a fixed listing and image identity:
 *
 *   - as is, which exercises the CRC checks and torn-line handling (§3.2, §6.1);
 *   - with a correct " *<CRC>" appended to each of its lines, so the fuzzer can
 *     explore the record grammar and the listing checks without forging CRCs.
 *
 * Checked after each read; a violation aborts: an invalid journal reports no done
 * files; a valid one only marks files of the listing, at most once each, and
 * matches the image (§4). Each is also read as a Deflate zip journal (§6.8): a valid one
 * has its done paths sorted, unique and findable, and its state within the image (§6.9). Seeds are
 * in core/tests/fuzz-seeds/journal/ (written without CRCs, like the second form). Built like
 * fuzz_http.c.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "xcat/crc32.h"
#include "xcat/journal.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static xc_xdvdfs_entry entries[] = {
    {"default.xbe", 65536, 12345, 32, 0},
    {"media/a b.bik", 131072, 4000000, 64, 0},
    {"media/100%.wav", 4325376, 0, 0, 0},
    {"x/y/z.dat", 4325376, 1, 2112, 0},
};
static xc_xdvdfs_listing listing = {entries, 4, NULL, 0, 0, NULL, NULL, 0, false};

static void check(int ok)
{
    if (!ok) {
        abort();
    }
}

static void read_and_check(const char *text, size_t len, const xc_journal_id *image)
{
    xc_journal_state st;
    xc_err err = xc_journal_read(text, len, &listing, image, &st);
    check(err == XC_OK);
    if (!st.valid) {
        check(st.why != NULL && st.done_count == 0 && !st.has_hash);
        for (size_t i = 0; i < listing.file_count; i++) {
            check(!st.done[i]);
        }
    } else {
        check(xc_journal_same_image(&st.id, image));
        size_t n = 0;
        for (size_t i = 0; i < listing.file_count; i++) {
            n += st.done[i] ? 1U : 0U;
        }
        check(n == st.done_count && n <= listing.file_count);
        /* §5 (v1.2): a hash state stands at a block boundary inside the image. */
        check(!st.has_hash ||
              (st.hash_offset % XC_HASH_BLOCK == 0 && st.hash_offset <= image->image_size));
    }
    xc_journal_state_free(&st);

    /* §6.8 (v1.3): the same text as a Deflate zip member's journal */
    xc_journal_id zip;
    xc_journal_make_zip_id(&zip, 786432, 0x2862EC8CU, 199506, "\"e\"", NULL);
    xc_journal_zip_state zs;
    err = xc_journal_read_zip(text, len, &zip, &zs);
    check(err == XC_OK);
    if (!zs.valid) {
        check(zs.why != NULL && zs.done_count == 0 && !zs.has_state && zs.drop_count == 0);
    } else {
        check(zs.id.zip && xc_journal_same_image(&zs.id, &zip));
        for (size_t i = 0; i < zs.done_count; i++) {
            check(zs.done[i].path != NULL && zs.done[i].path[0] != '\0');
            check(i == 0 || strcmp(zs.done[i - 1].path, zs.done[i].path) < 0);
            check(xc_journal_zip_find(&zs, zs.done[i].path) == &zs.done[i]);
        }
        /* §6.9 (v1.4): a state stands at a 32 KiB boundary inside the image */
        check(!zs.has_state || (zs.state_seq > 0 && zs.state_offset % XC_JOURNAL_STATE_ALIGN == 0 &&
                                zs.state_offset <= zip.image_size && zs.state_bytes > 0 &&
                                zs.state_bytes <= XC_JOURNAL_MAX_STATE));
        check(zs.drop_count == 0 || zs.drops != NULL);
    }
    xc_journal_zip_state_free(&zs);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > 65536) {
        return 0;
    }
    xc_journal_id image;
    xc_journal_make_id(&image, &listing, 0, 7825162240U, "\"e\"", NULL);
    read_and_check((const char *)data, size, &image);

    /* Each line gets its correct CRC: "<line> *XXXXXXXX\n". */
    char *with_crc = malloc((size + 1) * 12 + 16); /* every byte could end a line */
    if (with_crc == NULL) {
        return 0;
    }
    size_t out = 0;
    size_t start = 0;
    for (size_t i = 0; i <= size; i++) {
        if (i == size || data[i] == '\n') {
            if (i == size && start == size) {
                break;
            }
            static const char hex[] = "0123456789ABCDEF";
            size_t n = i - start;
            memcpy(with_crc + out, data + start, n);
            uint32_t crc = xc_crc32(0, data + start, n);
            out += n;
            with_crc[out++] = ' ';
            with_crc[out++] = '*';
            for (int k = 7; k >= 0; k--) {
                with_crc[out++] = hex[(crc >> (unsigned)(k * 4)) & 0xFU];
            }
            with_crc[out++] = '\n';
            start = i + 1;
        }
    }
    read_and_check(with_crc, out, &image);
    free(with_crc);
    return 0;
}
