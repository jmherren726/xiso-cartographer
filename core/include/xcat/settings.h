#ifndef XCAT_SETTINGS_H
#define XCAT_SETTINGS_H

/*
 * The settings file (docs/formats/settings.md, ADR 0016): what the user chose in SETTINGS that
 * isn't a login or a source. Version 1 holds one setting, the install location (§4.1).
 *
 * The file is all-or-nothing (§5): a problem anywhere rejects it, and every setting keeps its
 * default. The app writes it a key at a time (§6), keeping every other byte.
 */

#include <stddef.h>

#include "xcat/error.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XC_SETTINGS_MAX_FILE ((size_t)16 * 1024) /* §3.1 */
#define XC_SETTINGS_MAX_LINE 1024                /* §3.1, bytes without the line ending */
#define XC_SETTINGS_MAX_DIR 64                   /* §4.1, bytes from the '\' after the drive */
#define XC_SETTINGS_DIR_SIZE (2 + XC_SETTINGS_MAX_DIR + 1) /* "E:", the rest, the NUL */
#define XC_SETTINGS_DEFAULT_DIR "E:\\Games"                /* §4.1 */
#define XC_SETTINGS_VERSION_MAJOR 1

typedef struct xc_settings {
    char install_dir[XC_SETTINGS_DIR_SIZE]; /* §4.1, with the drive letter in capitals */
} xc_settings;

/* Why a file was rejected. */
typedef struct xc_settings_error {
    size_t line; /* 1-based; 0 for the file as a whole */
    char message[128];
} xc_settings_error;

/* Every setting at its default (§4). */
void xc_settings_default(xc_settings *s);

/*
 * Parses a settings file held in data[0, len). On XC_OK, `s` holds what it sets and the
 * defaults for the rest. On XC_ERR_SETTINGS `s` holds the defaults and `err` says where and
 * why. Keys this version doesn't know are skipped (§3.4).
 */
xc_err xc_settings_parse(const char *data, size_t len, xc_settings *s, xc_settings_error *err);

/*
 * NULL if `dir` can be the install location (§4.1), or why not, as words that follow the
 * folder's name ("must start with E:\, F:\ or G:\"). The drive letter may be either case.
 */
const char *xc_settings_check_dir(const char *dir);

/*
 * The file with `key = value` set (§6): data[0, len) with the key's line replaced, or the
 * line added at the end if there is none; with no file (data NULL, len 0), a new one. Every
 * other byte stays as it was. The file must parse before, and the result after, or it's
 * XC_ERR_SETTINGS and `err` says why. On success *out (malloc'd, NUL-terminated, *out_len
 * bytes) is the caller's to free.
 */
xc_err xc_settings_set(const char *data, size_t len, const char *key, const char *value, char **out,
                       size_t *out_len, xc_settings_error *err);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_SETTINGS_H */
