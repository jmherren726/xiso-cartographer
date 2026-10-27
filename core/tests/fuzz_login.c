/*
 * libFuzzer target for the on-box login (docs/formats/login.md).
 *
 * The first byte picks what the rest is: 'a' an answer of the login API (§3), 's' a
 * credentials file to write the [archive] section into (§5), 'r' one to remove it from.
 * Parsing an answer must give XC_OK, XC_ERR_CORRUPT or XC_ERR_LIMIT; a login has two
 * cookie-octet values, anything else none. A written file must parse, with the section there
 * or gone as asked. A violation aborts. Seeds are in core/tests/fuzz-seeds/login/.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "xcat/credentials.h"
#include "xcat/login.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void check(int ok)
{
    if (!ok) {
        abort();
    }
}

static void check_cookie(const char *s, size_t cap)
{
    check(memchr(s, '\0', cap) != NULL);
    check(s[0] != '\0');
    for (; *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;
        check(c > 0x20U && c < 0x7FU && strchr("\",;\\", c) == NULL);
    }
}

static void answer(const char *data, size_t size)
{
    xc_login_result r;
    xc_err err = xc_login_parse(data, size, &r);
    check(err == XC_OK || err == XC_ERR_CORRUPT || err == XC_ERR_LIMIT);
    check(memchr(r.reason, '\0', sizeof r.reason) != NULL);
    for (const char *p = r.reason; *p != '\0'; p++) {
        check(*p >= 0x20 && *p < 0x7F);
    }
    if (err == XC_OK && r.outcome == XC_LOGIN_OK) {
        check_cookie(r.user, sizeof r.user);
        check_cookie(r.sig, sizeof r.sig);
        char sec[2 * XC_LOGIN_MAX_COOKIE + 256];
        check(xc_login_section(&r, sec, sizeof sec, NULL) == XC_OK);
    } else {
        check(r.user[0] == '\0' && r.sig[0] == '\0');
    }
}

static void write_section(const char *data, size_t size, bool add)
{
    static const char sec[] = "[archive]\nhosts = archive.org, *.archive.org\n"
                              "cookie = logged-in-user=u; logged-in-sig=s\n";
    char *out = NULL;
    size_t len = 0;
    xc_cred_error e;
    xc_err err =
        xc_cred_set_section(data, size, XC_LOGIN_SECTION, add ? sec : NULL, &out, &len, &e);
    check(err == XC_OK || err == XC_ERR_CREDENTIALS || err == XC_ERR_LIMIT);
    if (err != XC_OK) {
        check(out == NULL);
        return;
    }
    check(out != NULL && out[len] == '\0');
    if (len > 0) {
        xc_credentials c;
        check(xc_cred_parse(out, len, &c, &e) == XC_OK);
        const xc_cred_section *s = xc_cred_section_named(&c, XC_LOGIN_SECTION);
        check(add ? s != NULL && strcmp(s->cookie, "logged-in-user=u; logged-in-sig=s") == 0
                  : s == NULL);
        xc_cred_free(&c);
    } else {
        check(!add);
    }
    free(out);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0) {
        return 0;
    }
    const char *rest = (const char *)data + 1;
    switch (data[0]) {
    case 'a':
        answer(rest, size - 1);
        break;
    case 's':
    case 'r':
        write_section(size > 1 ? rest : NULL, size - 1, data[0] == 's');
        break;
    default:
        break;
    }
    return 0;
}
