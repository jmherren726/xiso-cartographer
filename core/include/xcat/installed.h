#ifndef XCAT_INSTALLED_H
#define XCAT_INSTALLED_H

/*
 * What's already in the games folder (M8): reads the `.xcat-installed` marker
 * (docs/formats/journal.md §7.1) and tells, for one folder, whether it holds a finished
 * install, an unfinished one (a journal and no marker) or something else. The platform lists
 * the folders; this module only looks inside each, through xc_install_io's exists and
 * read_all. Nothing here decides how an install runs: xc_install still chooses its folder by
 * itself (ADR 0008).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xcat/error.h"
#include "xcat/fatx.h"
#include "xcat/hash.h"
#include "xcat/install.h"
#include "xcat/xbe.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XC_MARKER_MAX_FILE 8192 /* larger markers are refused (XC_ERR_LIMIT) */

/* A marker's values (journal.md §7.1). Strings are "" and has_* false when a key is absent. */
typedef struct xc_marker {
    char program[64];                /* the first line, e.g. "xiso-cartographer 0.8.0" */
    char image[XC_INSTALL_MAX_PATH]; /* the URL or path the image came from */
    bool has_files;
    uint64_t files;
    bool has_bytes;
    uint64_t bytes;
    char verified[32];              /* "crc32" or "crc32,image-hash" */
    char md5[2 * XC_MD5_BYTES + 1]; /* hex, as written */
    char sha1[2 * XC_SHA1_BYTES + 1];
    bool has_title_id;
    uint32_t title_id;
    char title[3 * XC_XBE_TITLE_UNITS + 1];
} xc_marker;

/*
 * Parses a marker. Lines end in LF (a CR before it is dropped); the first names the
 * program, the rest are key=value. Unknown keys and lines without '=' are ignored, a value
 * too long for its field is cut at a UTF-8 boundary, and a value that isn't UTF-8 text
 * without control characters, or a malformed number, hash or title ID, leaves that key
 * absent. XC_ERR_CORRUPT if the first line doesn't start with
 * "xiso-cartographer" or the text holds a NUL; XC_ERR_LIMIT over XC_MARKER_MAX_FILE.
 */
xc_err xc_marker_parse(const char *data, size_t len, xc_marker *m);

typedef enum xc_folder_state {
    XC_FOLDER_OTHER,      /* neither a marker nor a journal: not ours, never touched */
    XC_FOLDER_INSTALLED,  /* a marker: installed and verified */
    XC_FOLDER_UNFINISHED, /* a journal and no marker: installing again carries on */
} xc_folder_state;

typedef struct xc_installed {
    xc_folder_state state;
    bool marker_ok; /* XC_FOLDER_INSTALLED: the marker could be read and parsed */
    xc_marker marker;
} xc_installed;

/* Looks inside games_dir/folder. Errors only for paths too long (XC_ERR_LIMIT); a marker
   that can't be read or parsed still gives XC_FOLDER_INSTALLED, with marker_ok false. */
xc_err xc_installed_check(const xc_install_io *io, const char *games_dir, const char *folder,
                          xc_installed *out);

/* True if `folder` is a name xc_install gives an install of `title` (ADR 0008): its FATX
   name, or that name numbered "(2)" to "(99)". Compared without regard to ASCII case. */
bool xc_installed_folder_for(const char *folder, const char *title);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_INSTALLED_H */
