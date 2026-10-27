/*
 * Tests for the on-box login (xcat/login.h, docs/formats/login.md) and for writing one
 * credentials section (xc_cred_set_section, login.md §5). Every value is a placeholder.
 */
#include "unity.h"
#include "xcat/credentials.h"
#include "xcat/login.h"

#include <stdlib.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* --- §1: the body ------------------------------------------------------------------------ */

static void test_body(void)
{
    char out[XC_LOGIN_MAX_BODY];
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(
        XC_OK, xc_login_body("someone@example.org", "p a&s=s+%~", out, sizeof out, &len));
    TEST_ASSERT_EQUAL_STRING("email=someone%40example.org&password=p%20a%26s%3Ds%2B%25%7E", out);
    TEST_ASSERT_EQUAL_size_t(strlen(out), len);
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_login_body("a-b.c_d*", "\xC3\xA9", out, sizeof out, NULL));
    TEST_ASSERT_EQUAL_STRING("email=a-b.c_d*&password=%C3%A9", out);
}

static void test_body_rejects(void)
{
    char out[XC_LOGIN_MAX_BODY];
    char long_pw[XC_LOGIN_MAX_PASSWORD + 2];
    memset(long_pw, 'x', sizeof long_pw - 1);
    long_pw[sizeof long_pw - 1] = '\0';
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_login_body("", "pw", out, sizeof out, NULL));
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_login_body("e", "", out, sizeof out, NULL));
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_login_body("e", long_pw, out, sizeof out, NULL));
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_login_body("e\n", "pw", out, sizeof out, NULL));
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_login_body("e", "p\x7F", out, sizeof out, NULL));
    long_pw[XC_LOGIN_MAX_PASSWORD] = '\0'; /* the longest allowed, all 'x' */
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_login_body("e", long_pw, out, sizeof out, NULL));
    /* Too small a buffer: nothing of the password is left in it. */
    char small[24];
    TEST_ASSERT_EQUAL_INT(XC_ERR_LIMIT,
                          xc_login_body("e", "secretsecret", small, sizeof small, NULL));
    for (size_t i = 0; i < sizeof small; i++) {
        TEST_ASSERT_EQUAL_CHAR('\0', small[i]);
    }
}

/* --- §3: the answer ------------------------------------------------------------------------ */

static xc_err parse(const char *json, xc_login_result *r)
{
    return xc_login_parse(json, strlen(json), r);
}

static void test_parse_ok(void)
{
    xc_login_result r;
    TEST_ASSERT_EQUAL_INT(
        XC_OK, parse("{\"success\": true, \"values\": {\"cookies\": {\"logged-in-sig\": "
                     "\"SIG%20123\", \"logged-in-user\": \"someone@example.org\", \"other\": [1]},"
                     " \"s3\": {\"access\": \"A\", \"secret\": \"S\"}, \"screenname\": \"x\"},"
                     " \"version\": 1}",
                     &r));
    TEST_ASSERT_EQUAL_INT(XC_LOGIN_OK, r.outcome);
    TEST_ASSERT_EQUAL_STRING("someone@example.org", r.user);
    TEST_ASSERT_EQUAL_STRING("SIG%20123", r.sig);

    char sec[3 * XC_LOGIN_MAX_COOKIE];
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_login_section(&r, sec, sizeof sec, &len));
    TEST_ASSERT_EQUAL_STRING("[archive]\n"
                             "# Written by the on-box login. Log out in SETTINGS.\n"
                             "hosts = archive.org, *.archive.org\n"
                             "cookie = logged-in-user=someone@example.org; "
                             "logged-in-sig=SIG%20123\n",
                             sec);
    TEST_ASSERT_EQUAL_size_t(strlen(sec), len);
}

static void test_parse_refused(void)
{
    xc_login_result r;
    TEST_ASSERT_EQUAL_INT(
        XC_OK,
        parse("{\"success\": false, \"values\": {\"reason\": \"account_bad_password\"}}", &r));
    TEST_ASSERT_EQUAL_INT(XC_LOGIN_BAD_PASSWORD, r.outcome);
    TEST_ASSERT_EQUAL_INT(
        XC_OK, parse("{\"success\": false, \"values\": {\"reason\": \"account_not_found\"}}", &r));
    TEST_ASSERT_EQUAL_INT(XC_LOGIN_NO_ACCOUNT, r.outcome);
    /* values.reason wins over error, whichever comes first. */
    TEST_ASSERT_EQUAL_INT(XC_OK, parse("{\"error\": \"e\", \"success\": false, \"values\": "
                                       "{\"reason\": \"account_locked\"}}",
                                       &r));
    TEST_ASSERT_EQUAL_INT(XC_LOGIN_REFUSED, r.outcome);
    TEST_ASSERT_EQUAL_STRING("account_locked", r.reason);
    TEST_ASSERT_EQUAL_INT(XC_OK, parse("{\"success\": false, \"error\": \"try later\"}", &r));
    TEST_ASSERT_EQUAL_STRING("try later", r.reason);
    /* No success at all, and cookies that came anyway, aren't a login. */
    TEST_ASSERT_EQUAL_INT(XC_OK, parse("{\"values\": {\"cookies\": {\"logged-in-user\": \"u\", "
                                       "\"logged-in-sig\": \"s\"}}}",
                                       &r));
    TEST_ASSERT_EQUAL_INT(XC_LOGIN_REFUSED, r.outcome);
    TEST_ASSERT_EQUAL_STRING("", r.user);
    TEST_ASSERT_EQUAL_STRING("", r.sig);
    /* A reason is cut to plain text and XC_LOGIN_MAX_REASON. */
    TEST_ASSERT_EQUAL_INT(XC_OK, parse("{\"success\": false, \"error\": \"ab\\ncd\"}", &r));
    TEST_ASSERT_EQUAL_STRING("ab", r.reason);
    TEST_ASSERT_EQUAL_INT(
        XC_OK, parse("{\"success\": false, \"error\": "
                     "\"0123456789012345678901234567890123456789012345678901234567890123456789\"}",
                     &r));
    TEST_ASSERT_EQUAL_size_t(XC_LOGIN_MAX_REASON, strlen(r.reason));
}

static void test_parse_rejects(void)
{
    xc_login_result r;
    static const char *const bad[] = {
        "",
        "[]",
        "{\"success\": true}", /* no session */
        "{\"success\": true, \"values\": {\"cookies\": {\"logged-in-user\": \"u\"}}}",
        "{\"success\": true, \"values\": {\"cookies\": {\"logged-in-user\": \"u\", "
        "\"logged-in-sig\": \"a;b\"}}}",
        "{\"success\": true, \"values\": {\"cookies\": {\"logged-in-user\": \"u v\", "
        "\"logged-in-sig\": \"s\"}}}",
        "{\"success\": true, \"values\": {\"cookies\": {\"logged-in-user\": \"\", "
        "\"logged-in-sig\": \"s\"}}}",
        "{\"success\": true, \"values\": {\"cookies\": {\"logged-in-user\": 1, "
        "\"logged-in-sig\": \"s\"}}}",
        "{\"success\": false} x",
        "{\"success\": false",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(XC_ERR_CORRUPT, parse(bad[i], &r), bad[i]);
        TEST_ASSERT_EQUAL_STRING("", r.sig);
    }
    char *big = malloc(XC_LOGIN_MAX_RESPONSE + 1);
    TEST_ASSERT_NOT_NULL(big);
    memset(big, ' ', XC_LOGIN_MAX_RESPONSE + 1);
    TEST_ASSERT_EQUAL_INT(XC_ERR_LIMIT, xc_login_parse(big, XC_LOGIN_MAX_RESPONSE + 1, &r));
    free(big);
    r.outcome = XC_LOGIN_REFUSED;
    char sec[64];
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_login_section(&r, sec, sizeof sec, NULL));
}

/* --- §5: writing the section -------------------------------------------------------------- */

static const char section[] = "[archive]\nhosts = archive.org, *.archive.org\n"
                              "cookie = logged-in-user=u; logged-in-sig=s\n";

static char *set(const char *file, const char *sec, xc_err want)
{
    char *out = NULL;
    size_t len = 0;
    xc_cred_error err;
    xc_err e = xc_cred_set_section(file, file != NULL ? strlen(file) : 0, "archive", sec, &out,
                                   &len, &err);
    TEST_ASSERT_EQUAL_INT_MESSAGE(want, e, err.message);
    if (e == XC_OK) {
        TEST_ASSERT_EQUAL_size_t(strlen(out), len);
    }
    return out;
}

static void test_set_new_file(void)
{
    char *out = set(NULL, section, XC_OK);
    TEST_ASSERT_EQUAL_STRING("# xcat-credentials v1\n\n[archive]\nhosts = archive.org, "
                             "*.archive.org\ncookie = logged-in-user=u; logged-in-sig=s\n",
                             out);
    free(out);
    out = set(NULL, NULL, XC_OK); /* logging out with no file: still none */
    TEST_ASSERT_EQUAL_STRING("", out);
    free(out);
}

static const char file[] = "\xEF\xBB\xBF# xcat-credentials v1\r\n"
                           "# my NAS\r\n"
                           "[nas]\r\n"
                           "hosts = 192.168.1.20:8080\r\n"
                           "allow_http = yes\r\n"
                           "header = Authorization: Basic AAAA\r\n"
                           "\r\n"
                           "  [ARCHIVE]  \r\n"
                           "# old\r\n"
                           "hosts = archive.org, *.archive.org\r\n"
                           "\r\n"
                           "cookie = logged-in-user=old; logged-in-sig=old\r\n"
                           "\r\n"
                           "# the last one\r\n"
                           "[other]\r\n"
                           "hosts = example.org\r\n"
                           "cookie = a=b";

static void test_set_replaces(void)
{
    char *out = set(file, section, XC_OK);
    TEST_ASSERT_EQUAL_STRING("\xEF\xBB\xBF# xcat-credentials v1\r\n"
                             "# my NAS\r\n"
                             "[nas]\r\n"
                             "hosts = 192.168.1.20:8080\r\n"
                             "allow_http = yes\r\n"
                             "header = Authorization: Basic AAAA\r\n"
                             "\r\n"
                             "[archive]\nhosts = archive.org, *.archive.org\n"
                             "cookie = logged-in-user=u; logged-in-sig=s\n"
                             "\r\n"
                             "# the last one\r\n"
                             "[other]\r\n"
                             "hosts = example.org\r\n"
                             "cookie = a=b",
                             out);
    free(out);
}

static void test_set_removes_and_adds(void)
{
    char *out = set(file, NULL, XC_OK);
    TEST_ASSERT_NULL(strstr(out, "archive"));
    TEST_ASSERT_NOT_NULL(strstr(out, "# the last one\r\n[other]"));
    TEST_ASSERT_NOT_NULL(strstr(out, "Basic AAAA\r\n\r\n\r\n# the last one"));
    /* ... and added again at the end, after a blank line, the file's last line ended. */
    char *again = set(out, section, XC_OK);
    size_t n = strlen(out);
    TEST_ASSERT_EQUAL_INT(0, strncmp(again, out, n));
    TEST_ASSERT_EQUAL_STRING("\n\n[archive]\nhosts = archive.org, *.archive.org\n"
                             "cookie = logged-in-user=u; logged-in-sig=s\n",
                             again + n);
    free(again);
    free(out);
    /* Removing what isn't there changes nothing. */
    out = set("# xcat-credentials v1\n", NULL, XC_OK);
    TEST_ASSERT_EQUAL_STRING("# xcat-credentials v1\n", out);
    free(out);
}

static void test_set_refuses(void)
{
    /* A broken file is left alone: it isn't the login's to fix. */
    TEST_ASSERT_NULL(set("# xcat-credentials v1\n[x]\n", section, XC_ERR_CREDENTIALS));
    TEST_ASSERT_NULL(set("hosts = a\n", section, XC_ERR_CREDENTIALS));
    /* Another section already covers archive.org: the result would overlap. */
    TEST_ASSERT_NULL(set("# xcat-credentials v1\n[ia]\nhosts = archive.org\ncookie = a=b\n",
                         section, XC_ERR_CREDENTIALS));
    /* A section that isn't one. */
    TEST_ASSERT_NULL(set(NULL, "[archive]\nhosts = archive.org\n", XC_ERR_CREDENTIALS));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_body);
    RUN_TEST(test_body_rejects);
    RUN_TEST(test_parse_ok);
    RUN_TEST(test_parse_refused);
    RUN_TEST(test_parse_rejects);
    RUN_TEST(test_set_new_file);
    RUN_TEST(test_set_replaces);
    RUN_TEST(test_set_removes_and_adds);
    RUN_TEST(test_set_refuses);
    return UNITY_END();
}
