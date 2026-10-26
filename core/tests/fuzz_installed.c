/*
 * libFuzzer target for the install marker (docs/formats/journal.md §7.1).
 *
 * The input is a marker. Parsing must give XC_OK, XC_ERR_CORRUPT or XC_ERR_LIMIT, and on
 * success every string must be NUL-terminated UTF-8 without control characters, and the
 * hashes empty or hex of the right length. A violation aborts. Seeds are in
 * core/tests/fuzz-seeds/installed/.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "xcat/installed.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void check(int ok)
{
    if (!ok) {
        abort();
    }
}

static void check_text(const char *s, size_t cap)
{
    const char *nul = memchr(s, '\0', cap);
    check(nul != NULL);
    size_t n = (size_t)(nul - s);
    for (size_t i = 0; i < n;) {
        unsigned char c = (unsigned char)s[i];
        size_t k = c < 0x80U ? 1 : (c & 0xE0U) == 0xC0U ? 2 : (c & 0xF0U) == 0xE0U ? 3 : 4;
        check(i + k <= n && (k > 1 || (c >= 0x20U && c != 0x7FU)));
        for (size_t j = 1; j < k; j++) {
            check(((unsigned char)s[i + j] & 0xC0U) == 0x80U);
        }
        i += k;
    }
}

static void check_hex(const char *s, size_t want)
{
    size_t n = strlen(s);
    check(n == 0 || n == want);
    for (size_t i = 0; i < n; i++) {
        check(strchr("0123456789abcdefABCDEF", s[i]) != NULL);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    xc_marker m;
    xc_err err = xc_marker_parse((const char *)data, size, &m);
    check(err == XC_OK || err == XC_ERR_CORRUPT || err == XC_ERR_LIMIT);
    if (err != XC_OK) {
        return 0;
    }
    check(strncmp(m.program, "xiso-cartographer", 17) == 0 || m.program[0] == '\0');
    check_text(m.program, sizeof m.program);
    check_text(m.image, sizeof m.image);
    check_text(m.verified, sizeof m.verified);
    check_text(m.title, sizeof m.title);
    check_hex(m.md5, sizeof m.md5 - 1);
    check_hex(m.sha1, sizeof m.sha1 - 1);
    check(m.has_title_id || m.title_id == 0);
    check(m.has_files || m.files == 0);
    check(m.has_bytes || m.bytes == 0);
    return 0;
}
