/* Shared helpers for the line-based text formats (text.h). */
#include "text.h"

#include <stdint.h>
#include <string.h>

bool xc_text_utf8(const unsigned char *p, size_t len, size_t *bad_at)
{
    size_t i = 0;
    while (i < len) {
        unsigned char c = p[i];
        size_t n = 0;
        uint32_t cp = 0;
        uint32_t min = 0;
        if (c < 0x80U) {
            i++;
            continue;
        }
        if ((c & 0xE0U) == 0xC0U) {
            n = 1;
            cp = c & 0x1FU;
            min = 0x80U;
        } else if ((c & 0xF0U) == 0xE0U) {
            n = 2;
            cp = c & 0x0FU;
            min = 0x800U;
        } else if ((c & 0xF8U) == 0xF0U) {
            n = 3;
            cp = c & 0x07U;
            min = 0x10000U;
        } else {
            *bad_at = i;
            return false;
        }
        if (len - i <= n) {
            *bad_at = i;
            return false;
        }
        for (size_t k = 1; k <= n; k++) {
            if ((p[i + k] & 0xC0U) != 0x80U) {
                *bad_at = i;
                return false;
            }
            cp = (cp << 6U) | (p[i + k] & 0x3FU);
        }
        if (cp < min || cp > 0x10FFFFU || (cp >= 0xD800U && cp <= 0xDFFFU)) {
            *bad_at = i;
            return false;
        }
        i += n + 1;
    }
    return true;
}

bool xc_text_space(char c)
{
    return c == ' ' || c == '\t';
}

char *xc_text_trim(char *s)
{
    while (xc_text_space(*s)) {
        s++;
    }
    size_t n = strlen(s);
    while (n > 0 && xc_text_space(s[n - 1])) {
        s[--n] = '\0';
    }
    return s;
}

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

bool xc_text_eq_ci(const char *a, const char *b)
{
    for (; *a != '\0' && *b != '\0'; a++, b++) {
        if (lower(*a) != lower(*b)) {
            return false;
        }
    }
    return *a == *b;
}

/* "<digits>" without leading zeros (except "0"); advances *s. */
static bool version_number(const char **s, unsigned long *v)
{
    const char *p = *s;
    if (*p < '0' || *p > '9' || (p[0] == '0' && p[1] >= '0' && p[1] <= '9')) {
        return false;
    }
    unsigned long n = 0;
    for (; *p >= '0' && *p <= '9'; p++) {
        if (n > 100000UL) {
            return false;
        }
        n = n * 10U + (unsigned long)(*p - '0');
    }
    *s = p;
    *v = n;
    return true;
}

xc_text_header xc_text_check_header(const char *line, const char *name, unsigned long supported,
                                    unsigned long *major)
{
    size_t name_len = strlen(name);
    if (line[0] != '#' || !xc_text_space(line[1])) {
        return XC_TEXT_HEADER_MISSING;
    }
    const char *s = line + 1;
    while (xc_text_space(*s)) {
        s++;
    }
    if (strncmp(s, name, name_len) != 0 || !xc_text_space(s[name_len])) {
        return XC_TEXT_HEADER_MISSING;
    }
    s += name_len;
    while (xc_text_space(*s)) {
        s++;
    }
    if (*s != 'v') {
        return XC_TEXT_HEADER_MISSING;
    }
    s++;
    unsigned long minor = 0;
    if (!version_number(&s, major)) {
        return XC_TEXT_HEADER_MISSING;
    }
    if (*s == '.') {
        s++;
        if (!version_number(&s, &minor)) {
            return XC_TEXT_HEADER_MISSING;
        }
    }
    if (*s != '\0') {
        return XC_TEXT_HEADER_MISSING;
    }
    if (*major > supported) {
        return XC_TEXT_HEADER_NEWER;
    }
    return *major < supported ? XC_TEXT_HEADER_OLDER : XC_TEXT_HEADER_OK;
}
