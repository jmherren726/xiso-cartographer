#ifndef XCAT_LOGIN_H
#define XCAT_LOGIN_H

/*
 * Logging in to archive.org on the console (docs/formats/login.md, ADR 0010): the request
 * body for its login API, its answer, and the credentials section a login writes. The
 * password goes into the request body only, and xc_login_wipe() clears every copy.
 */

#include <stdbool.h>
#include <stddef.h>

#include "xcat/error.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XC_LOGIN_URL "https://archive.org/services/xauthn/?op=login" /* §1 */
#define XC_LOGIN_CONTENT_TYPE "application/x-www-form-urlencoded"    /* §1 */
#define XC_LOGIN_SECTION "archive"                                   /* §4 */
#define XC_LOGIN_MAX_EMAIL 254                                       /* §1, bytes */
#define XC_LOGIN_MAX_PASSWORD 128                                    /* §1, bytes */
#define XC_LOGIN_MAX_BODY (3 * (XC_LOGIN_MAX_EMAIL + XC_LOGIN_MAX_PASSWORD) + 32)
#define XC_LOGIN_MAX_RESPONSE ((size_t)16 * 1024) /* §3 */
#define XC_LOGIN_MAX_COOKIE 1024                  /* §3, each value's bytes */
#define XC_LOGIN_MAX_REASON 64                    /* §3 */

/* Overwrites n bytes at p in a way the compiler can't leave out. */
void xc_login_wipe(void *p, size_t n);

/* §1: "email=…&password=…", form-encoded, into out[0, cap) with a NUL; *len (may be NULL)
   gets its length. XC_ERR_INVALID_ARG for an empty or too long email or password, or one
   with a control character. The caller wipes `out` after the request. */
xc_err xc_login_body(const char *email, const char *password, char *out, size_t cap, size_t *len);

typedef enum xc_login_outcome {
    XC_LOGIN_OK,           /* logged in: user and sig hold the session */
    XC_LOGIN_NO_ACCOUNT,   /* "account_not_found" */
    XC_LOGIN_BAD_PASSWORD, /* "account_bad_password" */
    XC_LOGIN_REFUSED,      /* another reason, in `reason` ("" if none was given) */
} xc_login_outcome;

typedef struct xc_login_result {
    xc_login_outcome outcome;
    char user[XC_LOGIN_MAX_COOKIE + 1]; /* the logged-in-user cookie's value */
    char sig[XC_LOGIN_MAX_COOKIE + 1];  /* the logged-in-sig cookie's value: a secret */
    char reason[XC_LOGIN_MAX_REASON + 1];
} xc_login_result;

/* §3: reads the login API's answer. XC_ERR_CORRUPT if it isn't one (not JSON, or success
   without both cookies, or a cookie that isn't cookie-octets), XC_ERR_LIMIT if it's longer
   than XC_LOGIN_MAX_RESPONSE. The caller wipes `out` once the section is written. */
xc_err xc_login_parse(const char *data, size_t len, xc_login_result *out);

/* §4: the [archive] section for a successful login, into out[0, cap) with a NUL. */
xc_err xc_login_section(const xc_login_result *r, char *out, size_t cap, size_t *len);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_LOGIN_H */
