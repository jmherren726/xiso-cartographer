/*
 * libFuzzer target for the settings file (docs/formats/settings.md).
 *
 * The first byte picks what the rest is: 'p' a settings file to parse, 's' one to set
 * install_dir in (§6), 'd' a folder to check (§4.1). A parsed file gives XC_OK with a valid
 * install_dir, or XC_ERR_SETTINGS with the default; a written file must parse, with the value
 * set; a folder that checks out must be writable into a new file. A violation aborts. Seeds
 * are in core/tests/fuzz-seeds/settings/.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "xcat/settings.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void check(int ok)
{
    if (!ok) {
        abort();
    }
}

static void parse(const char *data, size_t size)
{
    xc_settings s;
    xc_settings_error e;
    xc_err err = xc_settings_parse(data, size, &s, &e);
    check(err == XC_OK || err == XC_ERR_SETTINGS);
    check(memchr(e.message, '\0', sizeof e.message) != NULL);
    check(memchr(s.install_dir, '\0', sizeof s.install_dir) != NULL);
    check(xc_settings_check_dir(s.install_dir) == NULL);
    check(s.install_dir[0] >= 'E' && s.install_dir[0] <= 'G');
    if (err != XC_OK) {
        check(strcmp(s.install_dir, XC_SETTINGS_DEFAULT_DIR) == 0);
    }
}

static void set(const char *data, size_t size, const char *value)
{
    char *out = NULL;
    size_t len = 0;
    xc_settings_error e;
    xc_err err = xc_settings_set(data, size, "install_dir", value, &out, &len, &e);
    check(err == XC_OK || err == XC_ERR_SETTINGS || err == XC_ERR_LIMIT);
    if (err != XC_OK) {
        check(out == NULL);
        return;
    }
    check(out != NULL && out[len] == '\0');
    xc_settings s;
    check(xc_settings_parse(out, len, &s, &e) == XC_OK);
    check(strcmp(s.install_dir, value) == 0);
    free(out);
}

static void dir(const char *data, size_t size)
{
    char d[256];
    if (size >= sizeof d || memchr(data, '\0', size) != NULL) {
        return;
    }
    memcpy(d, data, size);
    d[size] = '\0';
    if (xc_settings_check_dir(d) != NULL || d[0] > 'Z') {
        return; /* a small drive letter is written as it is, and read back in capitals */
    }
    for (const char *c = d; *c != '\0'; c++) {
        if ((unsigned char)*c < 0x20U || *c == 0x7F) {
            return; /* refused before the file is looked at */
        }
    }
    set(NULL, 0, d);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0) {
        return 0;
    }
    const char *rest = (const char *)data + 1;
    switch (data[0]) {
    case 'p':
        parse(size > 1 ? rest : NULL, size - 1);
        break;
    case 's':
        set(size > 1 ? rest : NULL, size - 1, "F:\\Fuzz games");
        break;
    case 'd':
        dir(rest, size - 1);
        break;
    default:
        break;
    }
    return 0;
}
