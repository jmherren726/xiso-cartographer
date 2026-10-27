/*
 * Logging in to archive.org (xcat/login.h): docs/formats/login.md §1 (the request body),
 * §3 (the answer) and §4 (the section written).
 */
#include "xcat/login.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "json.h"

void xc_login_wipe(void *p, size_t n)
{
    volatile uint8_t *v = p;
    while (n-- > 0) {
        *v++ = 0;
    }
}

/* §1: 1 to max bytes, no control characters. */
static bool field_ok(const char *s, size_t max)
{
    size_t n = 0;
    while (n <= max && s[n] != '\0') {
        n++;
    }
    if (n == 0 || n > max) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20U || c == 0x7FU) {
            return false;
        }
    }
    return true;
}

/* §1: every byte but A-Z a-z 0-9 - . _ * as %XX. */
static bool put_encoded(char *out, size_t cap, size_t *n, const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    for (; *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;
        bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                     c == '-' || c == '.' || c == '_' || c == '*';
        size_t need = plain ? 1U : 3U;
        if (cap - *n <= need) {
            return false;
        }
        if (plain) {
            out[(*n)++] = (char)c;
        } else {
            out[(*n)++] = '%';
            out[(*n)++] = hex[c >> 4U];
            out[(*n)++] = hex[c & 0x0FU];
        }
    }
    return true;
}

static bool put_text(char *out, size_t cap, size_t *n, const char *s)
{
    size_t k = strlen(s);
    if (cap - *n <= k) {
        return false;
    }
    memcpy(out + *n, s, k);
    *n += k;
    return true;
}

xc_err xc_login_body(const char *email, const char *password, char *out, size_t cap, size_t *len)
{
    if (email == NULL || password == NULL || out == NULL || cap == 0) {
        return XC_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    if (!field_ok(email, XC_LOGIN_MAX_EMAIL) || !field_ok(password, XC_LOGIN_MAX_PASSWORD)) {
        return XC_ERR_INVALID_ARG;
    }
    size_t n = 0;
    bool ok = put_text(out, cap, &n, "email=") && put_encoded(out, cap, &n, email) &&
              put_text(out, cap, &n, "&password=") && put_encoded(out, cap, &n, password);
    if (!ok) {
        xc_login_wipe(out, cap);
        return XC_ERR_LIMIT;
    }
    out[n] = '\0';
    if (len != NULL) {
        *len = n;
    }
    return XC_OK;
}

/* RFC 6265 §4.1.1 cookie-octet: printable ASCII but space, '"', ',', ';' and '\'. */
static bool cookie_ok(const char *s)
{
    if (*s == '\0') {
        return false;
    }
    for (; *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x21U || c > 0x7EU || c == '"' || c == ',' || c == ';' || c == '\\') {
            return false;
        }
    }
    return true;
}

/* §3: the reason, printable ASCII, cut to XC_LOGIN_MAX_REASON. */
static void take_reason(const xc_json_tok *t, char *out)
{
    char buf[256];
    out[0] = '\0';
    if (t->type != XC_JSON_STRING || xc_json_string(t, buf, sizeof buf, NULL) != XC_OK) {
        return;
    }
    size_t n = 0;
    for (const char *p = buf; *p != '\0' && n < XC_LOGIN_MAX_REASON; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20U || c > 0x7EU) {
            break; /* not plain text: keep what came before */
        }
        out[n++] = (char)c;
    }
    out[n] = '\0';
}

typedef struct seen {
    int success; /* -1 missing, 0 false, 1 true */
    bool user;
    bool sig;
    bool reason; /* values.reason, which wins over error */
} seen;

/* values.cookies: the two cookies, everything else skipped. */
static xc_err read_cookies(xc_json *j, xc_login_result *out, seen *s)
{
    xc_json_tok t;
    for (;;) {
        xc_err err = xc_json_next(j, &t);
        if (err != XC_OK || t.type == XC_JSON_OBJECT_END) {
            return err;
        }
        bool user = xc_json_equals(&t, "logged-in-user");
        bool sig = xc_json_equals(&t, "logged-in-sig");
        err = xc_json_next(j, &t);
        if (err != XC_OK) {
            return err;
        }
        if (user || sig) {
            char *buf = user ? out->user : out->sig;
            if (t.type != XC_JSON_STRING ||
                xc_json_string(&t, buf, XC_LOGIN_MAX_COOKIE + 1, NULL) != XC_OK ||
                !cookie_ok(buf)) {
                return XC_ERR_CORRUPT;
            }
            *(user ? &s->user : &s->sig) = true;
        } else {
            err = xc_json_skip(j, &t);
            if (err != XC_OK) {
                return err;
            }
        }
    }
}

/* values: cookies and reason, everything else skipped. */
static xc_err read_values(xc_json *j, xc_login_result *out, seen *s)
{
    xc_json_tok t;
    for (;;) {
        xc_err err = xc_json_next(j, &t);
        if (err != XC_OK || t.type == XC_JSON_OBJECT_END) {
            return err;
        }
        bool cookies = xc_json_equals(&t, "cookies");
        bool reason = xc_json_equals(&t, "reason");
        err = xc_json_next(j, &t);
        if (err != XC_OK) {
            return err;
        }
        if (cookies && t.type == XC_JSON_OBJECT) {
            err = read_cookies(j, out, s);
        } else if (reason && t.type == XC_JSON_STRING) {
            take_reason(&t, out->reason);
            s->reason = true;
        } else {
            err = xc_json_skip(j, &t);
        }
        if (err != XC_OK) {
            return err;
        }
    }
}

static xc_err read_answer(xc_json *j, xc_login_result *out, seen *s)
{
    xc_json_tok t;
    xc_err err = xc_json_next(j, &t);
    if (err != XC_OK) {
        return err;
    }
    if (t.type != XC_JSON_OBJECT) {
        return XC_ERR_CORRUPT;
    }
    for (;;) {
        err = xc_json_next(j, &t);
        if (err != XC_OK) {
            return err;
        }
        if (t.type == XC_JSON_OBJECT_END) {
            break;
        }
        bool success = xc_json_equals(&t, "success");
        bool values = xc_json_equals(&t, "values");
        bool error = xc_json_equals(&t, "error");
        err = xc_json_next(j, &t);
        if (err != XC_OK) {
            return err;
        }
        if (success && (t.type == XC_JSON_TRUE || t.type == XC_JSON_FALSE)) {
            s->success = t.type == XC_JSON_TRUE ? 1 : 0;
        } else if (values && t.type == XC_JSON_OBJECT) {
            err = read_values(j, out, s);
        } else if (error && t.type == XC_JSON_STRING && !s->reason) {
            take_reason(&t, out->reason);
        } else {
            err = xc_json_skip(j, &t);
        }
        if (err != XC_OK) {
            return err;
        }
    }
    err = xc_json_next(j, &t);
    if (err != XC_OK) {
        return err;
    }
    return t.type == XC_JSON_END ? XC_OK : XC_ERR_CORRUPT;
}

xc_err xc_login_parse(const char *data, size_t len, xc_login_result *out)
{
    if (out == NULL || (data == NULL && len > 0)) {
        return XC_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    if (len > XC_LOGIN_MAX_RESPONSE) {
        return XC_ERR_LIMIT;
    }
    xc_json j;
    xc_json_init(&j, data, len);
    seen s = {-1, false, false, false};
    xc_err err = read_answer(&j, out, &s);
    if (err == XC_OK && s.success == 1 && !(s.user && s.sig)) {
        err = XC_ERR_CORRUPT; /* §3: logged in, but without a session */
    }
    if (err != XC_OK) {
        xc_login_wipe(out, sizeof *out);
        return err == XC_ERR_LIMIT ? XC_ERR_CORRUPT : err;
    }
    if (s.success == 1) {
        out->outcome = XC_LOGIN_OK;
        out->reason[0] = '\0';
        return XC_OK;
    }
    xc_login_wipe(out->user, sizeof out->user);
    xc_login_wipe(out->sig, sizeof out->sig);
    out->outcome = strcmp(out->reason, "account_not_found") == 0      ? XC_LOGIN_NO_ACCOUNT
                   : strcmp(out->reason, "account_bad_password") == 0 ? XC_LOGIN_BAD_PASSWORD
                                                                      : XC_LOGIN_REFUSED;
    return XC_OK;
}

xc_err xc_login_section(const xc_login_result *r, char *out, size_t cap, size_t *len)
{
    if (r == NULL || out == NULL || cap == 0 || r->outcome != XC_LOGIN_OK || !cookie_ok(r->user) ||
        !cookie_ok(r->sig)) {
        return XC_ERR_INVALID_ARG;
    }
    int n = snprintf(out, cap,
                     "[" XC_LOGIN_SECTION "]\n"
                     "# Written by the on-box login. Log out in SETTINGS.\n"
                     "hosts = archive.org, *.archive.org\n"
                     "cookie = logged-in-user=%s; logged-in-sig=%s\n",
                     r->user, r->sig);
    if (n < 0 || (size_t)n >= cap) {
        xc_login_wipe(out, cap);
        return XC_ERR_LIMIT;
    }
    if (len != NULL) {
        *len = (size_t)n;
    }
    return XC_OK;
}
