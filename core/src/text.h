#ifndef XCAT_TEXT_H
#define XCAT_TEXT_H

/*
 * Helpers for the line-based text formats (map.md, credentials.md): strict UTF-8,
 * trimming, and the shared "# xcat-<format> v<MAJOR>[.<MINOR>]" header. Internal.
 */

#include <stdbool.h>
#include <stddef.h>

/* True if p[0, len) is valid UTF-8 (no overlongs, surrogates or > U+10FFFF);
 * otherwise *bad_at is the offset of the first bad sequence. */
bool xc_text_utf8(const unsigned char *p, size_t len, size_t *bad_at);

/* Spaces and tabs ("whitespace" in both formats). */
bool xc_text_space(char c);

/* Trims whitespace at both ends of a NUL-terminated string, in place. */
char *xc_text_trim(char *s);

/* ASCII case-insensitive string equality. */
bool xc_text_eq_ci(const char *a, const char *b);

typedef enum xc_text_header {
    XC_TEXT_HEADER_OK,
    XC_TEXT_HEADER_MISSING, /* not "# <name> v<version>" */
    XC_TEXT_HEADER_NEWER,   /* major > supported; *major says which */
    XC_TEXT_HEADER_OLDER,   /* major < supported */
} xc_text_header;

/*
 * Checks a trimmed header line "#" 1*WSP name 1*WSP "v" major ["." minor], with
 * numbers that have no leading zeros (map.md §3, credentials.md §3.3). `minor` may be
 * NULL; it is 0 when the header has none.
 */
xc_text_header xc_text_check_header(const char *line, const char *name, unsigned long supported,
                                    unsigned long *major, unsigned long *minor);

#endif /* XCAT_TEXT_H */
