/* The settings file (docs/formats/settings.md). */
#include "xcat/settings.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "text.h"
#include "xcat/fatx.h"

void xc_settings_default(xc_settings *s)
{
    (void)snprintf(s->install_dir, sizeof s->install_dir, "%s", XC_SETTINGS_DEFAULT_DIR);
}

/* §4.1 */
const char *xc_settings_check_dir(const char *dir)
{
    if (dir == NULL) {
        return "is missing";
    }
    char drive = dir[0] >= 'a' && dir[0] <= 'z' ? (char)(dir[0] - 'a' + 'A') : dir[0];
    if ((drive != 'E' && drive != 'F' && drive != 'G') || dir[1] != ':' || dir[2] != '\\') {
        return "must start with E:\\, F:\\ or G:\\";
    }
    const char *p = dir + 2;
    if (strlen(p) > XC_SETTINGS_MAX_DIR) {
        return "is longer than 64 characters after the drive";
    }
    if (p[1] == '\0') {
        return "is a whole drive: name a folder on it, like E:\\Games";
    }
    while (*p == '\\') {
        const char *name = p + 1;
        size_t n = strcspn(name, "\\");
        if (n == 0) {
            return "has an empty folder name (\\\\, or a \\ at the end)";
        }
        if (name[n - 1] == ' ') {
            return "has a folder name that ends with a space"; /* the file would trim it */
        }
        switch (xc_fatx_check_name(name, n, NULL)) {
        case XC_FATX_NAME_OK:
            break;
        case XC_FATX_NAME_TOO_LONG:
            return "has a folder name longer than 42 characters";
        case XC_FATX_NAME_DOTS:
            return "can't use . or .. as a folder name";
        case XC_FATX_NAME_EMPTY:
        case XC_FATX_NAME_BAD_BYTE:
        default:
            return "has a character the Xbox drive can't store";
        }
        p = name + n;
    }
    return NULL;
}

/* One line of data: [start, end) with its line ending, and its text, trimmed. */
typedef struct line_at {
    size_t start, end;
    const char *text;
    size_t text_len;
    size_t raw_len; /* without the line ending */
} line_at;

static bool next_line(const char *data, size_t len, size_t pos, line_at *l)
{
    if (pos >= len) {
        return false;
    }
    const char *nl = memchr(data + pos, '\n', len - pos);
    l->start = pos;
    l->end = nl != NULL ? (size_t)(nl - data) + 1U : len;
    size_t b = nl != NULL ? (size_t)(nl - data) : len;
    if (b > pos && data[b - 1] == '\r') {
        b--;
    }
    l->raw_len = b - pos;
    size_t a = pos;
    while (a < b && (data[a] == ' ' || data[a] == '\t')) {
        a++;
    }
    while (b > a && (data[b - 1] == ' ' || data[b - 1] == '\t')) {
        b--;
    }
    l->text = data + a;
    l->text_len = b - a;
    return true;
}

typedef struct parser {
    xc_settings_error *err;
    size_t line;
} parser;

static xc_err fail(parser *p, const char *fmt, ...)
{
    p->err->line = p->line;
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(p->err->message, sizeof p->err->message, fmt, ap);
    va_end(ap);
    return XC_ERR_SETTINGS;
}

/* §3.2: "# xcat-settings v<MAJOR>[.<MINOR>]". */
static xc_err check_header(parser *p, const char *line)
{
    unsigned long major = 0;
    switch (xc_text_check_header(line, "xcat-settings", XC_SETTINGS_VERSION_MAJOR, &major, NULL)) {
    case XC_TEXT_HEADER_OK:
        return XC_OK;
    case XC_TEXT_HEADER_NEWER:
        return fail(p, "needs a newer xiso-cartographer (settings format v%lu)", major);
    case XC_TEXT_HEADER_OLDER:
        return fail(p, "unsupported settings format v%lu", major);
    case XC_TEXT_HEADER_MISSING:
        break;
    }
    return fail(p, "not a settings file (missing \"# xcat-settings v1\")");
}

static bool key_ok(const char *k)
{
    if (*k == '\0') {
        return false;
    }
    for (; *k != '\0'; k++) {
        if (!((*k >= 'a' && *k <= 'z') || *k == '_')) {
            return false;
        }
    }
    return true;
}

/* §3.3, §4: one "key = value" line, already trimmed and NUL-terminated. */
static xc_err key_value(parser *p, char *line, xc_settings *s, bool *have_dir)
{
    for (const char *c = line; *c != '\0'; c++) {
        if (((unsigned char)*c < 0x20U && *c != '\t') || *c == 0x7F) {
            return fail(p, "a line has a control character");
        }
    }
    char *eq = strchr(line, '=');
    if (eq == NULL) {
        return fail(p, "a line is neither a comment nor \"key = value\"");
    }
    *eq = '\0';
    char *key = xc_text_trim(line);
    char *value = xc_text_trim(eq + 1);
    if (!key_ok(key)) {
        return fail(p, "a key name may only have a-z and _");
    }
    if (value[0] == '\0') {
        return fail(p, "%s has no value", key);
    }
    if (strcmp(key, "install_dir") == 0) {
        if (*have_dir) {
            return fail(p, "install_dir is given twice");
        }
        const char *why = xc_settings_check_dir(value);
        if (why != NULL) {
            return fail(p, "install_dir %s", why);
        }
        (void)snprintf(s->install_dir, sizeof s->install_dir, "%s", value);
        if (s->install_dir[0] >= 'a') {
            s->install_dir[0] = (char)(s->install_dir[0] - 'a' + 'A');
        }
        *have_dir = true;
    }
    return XC_OK; /* §3.4: other keys are skipped */
}

xc_err xc_settings_parse(const char *data, size_t len, xc_settings *s, xc_settings_error *err)
{
    if (s == NULL || err == NULL || (data == NULL && len > 0)) {
        return XC_ERR_INVALID_ARG;
    }
    xc_settings_default(s);
    memset(err, 0, sizeof *err);
    parser p = {err, 0};
    if (len > XC_SETTINGS_MAX_FILE) {
        return fail(&p, "file larger than 16 KiB");
    }
    if (len >= 3 && (unsigned char)data[0] == 0xEFU && (unsigned char)data[1] == 0xBBU &&
        (unsigned char)data[2] == 0xBFU) {
        data += 3;
        len -= 3;
    }
    /* The first byte that isn't UTF-8 text, or a NUL, whichever comes first. */
    size_t at = len;
    (void)xc_text_utf8((const unsigned char *)data, len, &at);
    const char *nul = len > 0 ? memchr(data, '\0', len) : NULL;
    if (nul != NULL && (size_t)(nul - data) < at) {
        at = (size_t)(nul - data);
    }
    if (at < len) {
        p.line = 1;
        for (size_t i = 0; i < at; i++) {
            p.line += data[i] == '\n' ? 1U : 0U;
        }
        return fail(&p, "not UTF-8 text");
    }
    xc_settings got;
    xc_settings_default(&got);
    bool header_seen = false;
    bool have_dir = false;
    char buf[XC_SETTINGS_MAX_LINE + 1];
    line_at l;
    for (size_t pos = 0; next_line(data, len, pos, &l); pos = l.end) {
        p.line++;
        if (l.raw_len > XC_SETTINGS_MAX_LINE) {
            return fail(&p, "line longer than %d bytes", XC_SETTINGS_MAX_LINE);
        }
        if (l.text_len == 0) {
            continue;
        }
        memcpy(buf, l.text, l.text_len);
        buf[l.text_len] = '\0';
        xc_err e = XC_OK;
        if (!header_seen) {
            e = check_header(&p, buf);
            header_seen = true;
        } else if (buf[0] != '#') {
            e = key_value(&p, buf, &got, &have_dir);
        }
        if (e != XC_OK) {
            return e;
        }
    }
    if (!header_seen) {
        p.line = 0;
        return fail(&p, "empty file (missing \"# xcat-settings v1\")");
    }
    *s = got;
    return XC_OK;
}

/* True if line l is "key = ..." for this key. */
static bool sets_key(const line_at *l, const char *key)
{
    if (l->text_len == 0 || l->text[0] == '#') {
        return false;
    }
    const char *eq = memchr(l->text, '=', l->text_len);
    if (eq == NULL) {
        return false;
    }
    size_t n = (size_t)(eq - l->text);
    while (n > 0 && (l->text[n - 1] == ' ' || l->text[n - 1] == '\t')) {
        n--;
    }
    return n == strlen(key) && memcmp(l->text, key, n) == 0;
}

static bool append(char *buf, size_t *n, size_t cap, const char *s, size_t len)
{
    if (len > cap - *n) {
        return false;
    }
    if (len > 0) {
        memcpy(buf + *n, s, len);
        *n += len;
    }
    return true;
}

xc_err xc_settings_set(const char *data, size_t len, const char *key, const char *value, char **out,
                       size_t *out_len, xc_settings_error *err)
{
    if (key == NULL || value == NULL || out == NULL || out_len == NULL || err == NULL ||
        (data == NULL && len > 0) || !key_ok(key)) {
        return XC_ERR_INVALID_ARG;
    }
    for (const char *c = value; *c != '\0'; c++) {
        if ((unsigned char)*c < 0x20U || *c == 0x7F) {
            return XC_ERR_INVALID_ARG; /* a line break would write another line */
        }
    }
    *out = NULL;
    *out_len = 0;
    memset(err, 0, sizeof *err);
    xc_settings s;
    if (len > 0) {
        xc_err e = xc_settings_parse(data, len, &s, err);
        if (e != XC_OK) {
            return e;
        }
    }
    static const char header[] = "# xcat-settings v1\n\n";
    size_t key_len = strlen(key);
    size_t value_len = strlen(value);
    size_t cap = len + sizeof header + key_len + value_len + 8U;
    char *buf = malloc(cap + 1U);
    if (buf == NULL) {
        return XC_ERR_NOMEM;
    }
    /* §6: the key's last line is replaced; without one, the line goes at the end. */
    bool found = false;
    line_at at = {0};
    line_at l;
    for (size_t pos = 0; next_line(data, len, pos, &l); pos = l.end) {
        if (sets_key(&l, key)) {
            found = true;
            at = l;
        }
    }
    size_t n = 0;
    bool ok = true;
    if (len == 0) {
        ok = append(buf, &n, cap, header, sizeof header - 1U);
    } else if (found) {
        ok = append(buf, &n, cap, data, at.start);
    } else {
        ok = append(buf, &n, cap, data, len) &&
             (data[len - 1] == '\n' || append(buf, &n, cap, "\n", 1));
    }
    ok = ok && append(buf, &n, cap, key, key_len) && append(buf, &n, cap, " = ", 3) &&
         append(buf, &n, cap, value, value_len) && append(buf, &n, cap, "\n", 1);
    if (ok && found) {
        ok = append(buf, &n, cap, data + at.end, len - at.end);
    }
    buf[n] = '\0';
    xc_err e = ok ? XC_OK : XC_ERR_LIMIT;
    if (e == XC_OK) {
        e = xc_settings_parse(buf, n, &s, err); /* the result must be a valid file too */
    }
    if (e != XC_OK) {
        free(buf);
        return e;
    }
    *out = buf;
    *out_len = n;
    return XC_OK;
}
