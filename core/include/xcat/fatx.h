#ifndef XCAT_FATX_H
#define XCAT_FATX_H

/*
 * FATX naming rules, as measured on the console's kernel by app/fatx-probe
 * (docs/formats/fatx-names.md), and the checks built on them (ADR 0008):
 * a game whose own files break the rules is refused, never renamed; the
 * install folder name is ours to choose and is made valid.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xcat/error.h"
#include "xcat/xdvdfs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XC_FATX_MAX_NAME 42 /* bytes per file or directory name */
/* bytes of a path counted from the first '\' after the drive, e.g. "\Games\Halo\default.xbe" */
#define XC_FATX_MAX_PATH 250
#define XC_FATX_MAX_DIR_ENTRIES 4096 /* files + directories in one directory */
/*
 * Largest file the kernel lets a file grow to on E: (16 KiB clusters): 4 GiB minus one
 * cluster. Writing past it fails with STATUS_FILE_CORRUPT_ERROR (fatx-names.md §4b).
 */
#define XC_FATX_MAX_FILE_SIZE 4294950912U

/* True if the kernel accepts byte `c` in a name (fatx-names.md §2). */
bool xc_fatx_byte_ok(uint8_t c);

typedef enum xc_fatx_name_check {
    XC_FATX_NAME_OK,
    XC_FATX_NAME_EMPTY,
    XC_FATX_NAME_TOO_LONG, /* more than XC_FATX_MAX_NAME bytes */
    XC_FATX_NAME_BAD_BYTE, /* a byte the kernel rejects */
    XC_FATX_NAME_DOTS      /* "." or "..", which are path components, not names */
} xc_fatx_name_check;

/* Checks one name of `len` bytes. On XC_FATX_NAME_BAD_BYTE, *bad_index (if not NULL) is set. */
xc_fatx_name_check xc_fatx_check_name(const char *name, size_t len, size_t *bad_index);

typedef enum xc_fatx_problem_kind {
    XC_FATX_PROBLEM_NAME,             /* `name` says why; `value` = bad byte index if any */
    XC_FATX_PROBLEM_PATH_TOO_LONG,    /* `value` = the full path length on the drive */
    XC_FATX_PROBLEM_TOO_MANY_ENTRIES, /* `path` = directory ("" = root); `value` = count */
    XC_FATX_PROBLEM_FILE_TOO_BIG      /* `value` = the file's size (over XC_FATX_MAX_FILE_SIZE) */
} xc_fatx_problem_kind;

typedef struct xc_fatx_problem {
    xc_fatx_problem_kind kind;
    xc_fatx_name_check name;
    const char *path; /* points into the listing (or "" for the install root) */
    size_t value;
} xc_fatx_problem;

/*
 * Checks every entry of a listing against the FATX rules, as if installed under a
 * directory whose path (from the first '\' after the drive, e.g. "\Games\Halo") is
 * `install_dir_len` bytes long. Stores up to `max_problems` problems and sets
 * *problem_count to the total found; 0 means the game can be installed.
 * Returns XC_OK unless the arguments are invalid or memory runs out.
 */
xc_err xc_fatx_check_listing(const xc_xdvdfs_listing *listing, size_t install_dir_len,
                             xc_fatx_problem *problems, size_t max_problems, size_t *problem_count);

/*
 * Makes a valid FATX name of 1..max_len bytes from any title (e.g. a catalog title or
 * image file name): ": " becomes " - ", each run of other invalid or non-ASCII bytes
 * becomes one '_', leading and trailing spaces, dots and '_' are trimmed, and an empty
 * result becomes "Game". `max_len` is clamped to 1..XC_FATX_MAX_NAME; `out` must hold
 * max_len + 1 bytes. Returns the length written.
 */
size_t xc_fatx_title_name(const char *title, char *out, size_t max_len);

/*
 * Writes "<base> (<n>)" for n >= 2, shortening `base` so the result fits in
 * XC_FATX_MAX_NAME bytes (for picking a folder name that isn't taken). `base` must be
 * a valid name, e.g. from xc_fatx_title_name(). `out` must hold XC_FATX_MAX_NAME + 1
 * bytes. Returns the length written.
 */
size_t xc_fatx_numbered_name(const char *base, unsigned n, char *out);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_FATX_H */
