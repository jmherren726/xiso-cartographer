/* The UI script (docs/formats/ui-script.md). */
#include "xcat/ui_script.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "text.h"

static const char *const key_names[XC_UI_KEY_COUNT_] = {
    "up", "down",  "left", "right", "a",  "b",     "x",
    "y",  "start", "back", "lt",    "rt", "white", "black",
};

const char *xc_ui_key_name(xc_ui_key key)
{
    return (unsigned)key < XC_UI_KEY_COUNT_ ? key_names[key] : "?";
}

typedef struct parser {
    xc_ui_script_error *err;
    size_t line;
} parser;

static xc_err fail(parser *p, const char *fmt, ...)
{
    p->err->line = p->line;
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(p->err->message, sizeof p->err->message, fmt, ap);
    va_end(ap);
    return XC_ERR_UI_SCRIPT;
}

/* §3.2: "# xcat-ui-script v<MAJOR>[.<MINOR>]". */
static xc_err check_header(parser *p, const char *line)
{
    unsigned long major = 0;
    switch (
        xc_text_check_header(line, "xcat-ui-script", XC_UI_SCRIPT_VERSION_MAJOR, &major, NULL)) {
    case XC_TEXT_HEADER_OK:
        return XC_OK;
    case XC_TEXT_HEADER_NEWER:
        return fail(p, "needs a newer xiso-cartographer (UI script format v%lu)", major);
    case XC_TEXT_HEADER_OLDER:
        return fail(p, "unsupported UI script format v%lu", major);
    case XC_TEXT_HEADER_MISSING:
        break;
    }
    return fail(p, "not a UI script (missing \"# xcat-ui-script v1\")");
}

/* The next word of *s, NUL-terminated in place, or NULL at the end; *s moves past it. */
static char *word(char **s)
{
    char *a = *s;
    while (xc_text_space(*a)) {
        a++;
    }
    if (*a == '\0') {
        *s = a;
        return NULL;
    }
    char *b = a;
    while (*b != '\0' && !xc_text_space(*b)) {
        b++;
    }
    if (*b != '\0') {
        *b++ = '\0';
    }
    *s = b;
    return a;
}

/* §3.4: a decimal number in [min, max], no sign and no leading zeros. */
static bool number(const char *w, uint32_t min, uint32_t max, uint32_t *out)
{
    size_t n = strlen(w);
    if (n == 0 || n > 7 || (n > 1 && w[0] == '0')) {
        return false;
    }
    uint32_t v = 0;
    for (size_t i = 0; i < n; i++) {
        if (w[i] < '0' || w[i] > '9') {
            return false;
        }
        v = v * 10U + (uint32_t)(w[i] - '0');
    }
    if (v < min || v > max) {
        return false;
    }
    *out = v;
    return true;
}

/* §4.3: 1 to 31 of a-z, 0-9 and '-'. */
static bool screen_name(const char *w)
{
    size_t n = strlen(w);
    if (n == 0 || n > XC_UI_SCRIPT_SCREEN_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = w[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
    }
    return true;
}

/* §4: one command line, already trimmed and NUL-terminated. */
static xc_err command(parser *p, char *line, xc_ui_step *st)
{
    for (const char *c = line; *c != '\0'; c++) {
        if (((unsigned char)*c < 0x20U && *c != '\t') || *c == 0x7F) {
            return fail(p, "a line has a control character");
        }
    }
    memset(st, 0, sizeof *st);
    st->line = p->line;
    char *rest = line;
    const char *cmd = word(&rest);
    if (cmd == NULL) {
        return fail(p, "an empty command"); /* not reached: blank lines are skipped */
    }
    if (xc_text_eq_ci(cmd, "note")) {
        char *text = xc_text_trim(rest);
        if (text[0] == '\0') {
            return fail(p, "note has no text");
        }
        if (strlen(text) >= sizeof st->text) {
            return fail(p, "a note is longer than %d bytes", (int)sizeof st->text - 1);
        }
        st->op = XC_UI_NOTE;
        memcpy(st->text, text, strlen(text) + 1U);
        return XC_OK;
    }
    const char *a1 = word(&rest);
    const char *a2 = word(&rest);
    if (word(&rest) != NULL) {
        return fail(p, "%s has too many words", cmd);
    }
    if (xc_text_eq_ci(cmd, "press")) {
        if (a1 == NULL) {
            return fail(p, "press needs a button");
        }
        st->op = XC_UI_PRESS;
        st->key = XC_UI_KEY_COUNT_;
        for (int k = 0; k < XC_UI_KEY_COUNT_; k++) {
            if (xc_text_eq_ci(a1, key_names[k])) {
                st->key = (xc_ui_key)k;
            }
        }
        if (st->key == XC_UI_KEY_COUNT_) {
            return fail(p, "unknown button \"%.32s\"", a1);
        }
        st->count = 1;
        if (a2 != NULL && !number(a2, 1, XC_UI_SCRIPT_MAX_PRESSES, &st->count)) {
            return fail(p, "a press's count must be 1 to %u", XC_UI_SCRIPT_MAX_PRESSES);
        }
        return XC_OK;
    }
    if (xc_text_eq_ci(cmd, "wait")) {
        if (a2 != NULL) {
            return fail(p, "wait has too many words");
        }
        if (a1 == NULL || !number(a1, 0, XC_UI_SCRIPT_MAX_WAIT_MS, &st->ms)) {
            return fail(p, "wait needs a time of 0 to %u ms", XC_UI_SCRIPT_MAX_WAIT_MS);
        }
        st->op = XC_UI_WAIT;
        return XC_OK;
    }
    if (xc_text_eq_ci(cmd, "expect")) {
        if (a1 == NULL || !screen_name(a1)) {
            return fail(p, "expect needs a screen's name: 1 to %d of a-z, 0-9 and -",
                        XC_UI_SCRIPT_SCREEN_MAX);
        }
        st->op = XC_UI_EXPECT;
        memcpy(st->text, a1, strlen(a1) + 1U);
        st->ms = XC_UI_SCRIPT_EXPECT_MS;
        if (a2 != NULL && !number(a2, 1, XC_UI_SCRIPT_MAX_EXPECT_MS, &st->ms)) {
            return fail(p, "expect's time must be 1 to %u ms", XC_UI_SCRIPT_MAX_EXPECT_MS);
        }
        return XC_OK;
    }
    return fail(p, "unknown command \"%.32s\"", cmd);
}

xc_err xc_ui_script_parse(const char *data, size_t len, xc_ui_script *s, xc_ui_script_error *err)
{
    if (s == NULL || err == NULL || (data == NULL && len > 0)) {
        return XC_ERR_INVALID_ARG;
    }
    s->count = 0;
    memset(err, 0, sizeof *err);
    parser p = {err, 0};
    if (len > XC_UI_SCRIPT_MAX_FILE) {
        return fail(&p, "file larger than 64 KiB");
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
    bool header_seen = false;
    char buf[XC_UI_SCRIPT_MAX_LINE + 1];
    size_t pos = 0;
    xc_err e = XC_OK;
    while (e == XC_OK && pos < len) {
        p.line++;
        const char *nl = memchr(data + pos, '\n', len - pos);
        size_t end = nl != NULL ? (size_t)(nl - data) : len;
        size_t n = end - pos;
        if (n > 0 && data[end - 1] == '\r') {
            n--;
        }
        if (n > XC_UI_SCRIPT_MAX_LINE) {
            e = fail(&p, "line longer than %d bytes", XC_UI_SCRIPT_MAX_LINE);
            break;
        }
        if (n > 0) {
            memcpy(buf, data + pos, n);
        }
        buf[n] = '\0';
        pos = nl != NULL ? end + 1U : len;
        char *text = xc_text_trim(buf);
        if (text[0] == '\0') {
            continue;
        }
        if (!header_seen) {
            e = check_header(&p, text);
            header_seen = true;
        } else if (text[0] != '#') {
            if (s->count == XC_UI_SCRIPT_MAX_STEPS) {
                e = fail(&p, "more than %d commands", XC_UI_SCRIPT_MAX_STEPS);
            } else {
                e = command(&p, text, &s->steps[s->count]);
                s->count += e == XC_OK ? 1U : 0U;
            }
        }
    }
    if (e == XC_OK && !header_seen) {
        p.line = 0;
        e = fail(&p, "empty file (missing \"# xcat-ui-script v1\")");
    }
    if (e != XC_OK) {
        s->count = 0;
    }
    return e;
}
