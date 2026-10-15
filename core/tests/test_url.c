/* Tests for URL parsing and redirect resolution (xcat/url.h, docs/formats/http.md §2). */
#include "unity.h"
#include "xcat/url.h"

#include <stdio.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static xc_url u;

static xc_err parse(const char *s)
{
    memset(&u, 0xAB, sizeof u);
    return xc_url_parse(s, strlen(s), &u);
}

static void test_basic(void)
{
    TEST_ASSERT_EQUAL_INT(XC_OK, parse("http://192.168.1.20:8080/xbox/a.iso"));
    TEST_ASSERT_EQUAL_INT(XC_URL_HTTP, u.scheme);
    TEST_ASSERT_EQUAL_STRING("192.168.1.20", u.host);
    TEST_ASSERT_EQUAL_UINT16(8080, u.port);
    TEST_ASSERT_TRUE(u.is_ipv4);
    TEST_ASSERT_FALSE(xc_url_default_port(&u));
    TEST_ASSERT_EQUAL_STRING("/xbox/a.iso", u.target);
    uint8_t ip[4];
    TEST_ASSERT_TRUE(xc_url_ipv4(&u, ip));
    TEST_ASSERT_EQUAL_UINT8(192, ip[0]);
    TEST_ASSERT_EQUAL_UINT8(20, ip[3]);

    TEST_ASSERT_EQUAL_INT(XC_OK, parse("HTTPS://Example.ORG"));
    TEST_ASSERT_EQUAL_INT(XC_URL_HTTPS, u.scheme);
    TEST_ASSERT_EQUAL_STRING("example.org", u.host);
    TEST_ASSERT_EQUAL_UINT16(443, u.port);
    TEST_ASSERT_TRUE(xc_url_default_port(&u));
    TEST_ASSERT_FALSE(u.is_ipv4);
    TEST_ASSERT_FALSE(xc_url_ipv4(&u, ip));
    TEST_ASSERT_EQUAL_STRING("/", u.target);

    TEST_ASSERT_EQUAL_INT(XC_OK, parse("http://nas?x=1"));
    TEST_ASSERT_EQUAL_UINT16(80, u.port);
    TEST_ASSERT_EQUAL_STRING("/?x=1", u.target);

    TEST_ASSERT_EQUAL_INT(XC_OK, parse("http://a-b.c.d:1/p?q=/x/../y"));
    TEST_ASSERT_EQUAL_STRING("/p?q=/x/../y", u.target); /* the query is left alone */
}

/* §2.2: map.md allows spaces and non-ASCII; they are encoded, %XX is kept. */
static void test_percent_encoding(void)
{
    TEST_ASSERT_EQUAL_INT(XC_OK, parse("http://h/xbox/Halo (Europe).xiso.iso"));
    TEST_ASSERT_EQUAL_STRING("/xbox/Halo%20(Europe).xiso.iso", u.target);
    TEST_ASSERT_EQUAL_INT(XC_OK, parse("http://h/a%20b%2fc"));
    TEST_ASSERT_EQUAL_STRING("/a%20b%2fc", u.target);
    TEST_ASSERT_EQUAL_INT(XC_OK, parse("http://h/100%/x%4"));
    TEST_ASSERT_EQUAL_STRING("/100%25/x%254", u.target);
    TEST_ASSERT_EQUAL_INT(XC_OK, parse("http://h/%"));
    TEST_ASSERT_EQUAL_STRING("/%25", u.target);
    TEST_ASSERT_EQUAL_INT(XC_OK, parse("http://h/%%41"));
    TEST_ASSERT_EQUAL_STRING("/%25%41", u.target);
    TEST_ASSERT_EQUAL_INT(XC_OK, parse("http://h/\xC3\xA9\"<>\\^`{|}?a b"));
    TEST_ASSERT_EQUAL_STRING("/%C3%A9%22%3C%3E%5C%5E%60%7B%7C%7D?a%20b", u.target);
}

static void test_dot_segments(void)
{
    static const char *const cases[][2] = {
        {"http://h/a/b/../c", "/a/c"}, {"http://h/a/./b", "/a/b"},
        {"http://h/a/b/..", "/a/"},    {"http://h/a/.", "/a/"},
        {"http://h/../../x", "/x"},    {"http://h/..", "/"},
        {"http://h/a//b/", "/a//b/"},  {"http://h/a/..b/.c", "/a/..b/.c"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(XC_OK, parse(cases[i][0]), cases[i][0]);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(cases[i][1], u.target, cases[i][0]);
    }
}

static void test_rejected(void)
{
    static const char *const bad[] = {
        "",
        "ftp://h/x",
        "http:/h/x",
        "http//h",
        "http://",
        "http:///x",
        "http://user:pw@h/x", /* credentials never come from a URL */
        "http://user@h/x",
        "http://[::1]/x", /* IPv6: not in v1 */
        "http://h:0/",
        "http://h:65536/",
        "http://h:123456/",
        "http://h:/",
        "http://h:8a/",
        "http://h/x#frag", /* fragments must be %23 */
        "http://h/a\tb",
        "http://h/a\x7f",
        "http://h_x/",
        "http://.h/",
        "http://h./",
        "http://a..b/",
        "http://1.2.3/", /* numeric last label: must be a real IPv4 address */
        "http://256.1.1.1/",
        "http://01.2.3.4/",
        "http://1.2.3.4.5/",
        "http://0x7f.1/",
        "http://example.123/",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(XC_ERR_URL, parse(bad[i]), bad[i]);
    }
    TEST_ASSERT_EQUAL_INT(XC_OK, parse("http://0.0.0.0/"));
    TEST_ASSERT_EQUAL_INT(XC_OK, parse("http://x1.example/"));
    TEST_ASSERT_EQUAL_INT(XC_OK, parse("http://h:65535/"));
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_url_parse(NULL, 0, &u));
}

static void test_limits(void)
{
    static char long_url[XC_URL_MAX + 2];
    memcpy(long_url, "http://h/", 9);
    memset(long_url + 9, 'a', XC_URL_MAX - 9);
    long_url[XC_URL_MAX] = '\0';
    TEST_ASSERT_EQUAL_INT(XC_OK, parse(long_url));
    long_url[XC_URL_MAX] = 'a';
    long_url[XC_URL_MAX + 1] = '\0';
    TEST_ASSERT_EQUAL_INT(XC_ERR_URL, parse(long_url));

    /* 2000 spaces encode to 6000 bytes: over the target limit. */
    memset(long_url + 9, ' ', 2000);
    long_url[2009] = '\0';
    TEST_ASSERT_EQUAL_INT(XC_ERR_URL, parse(long_url));
    long_url[9 + 1300] = '\0'; /* 3900 bytes: fits */
    TEST_ASSERT_EQUAL_INT(XC_OK, parse(long_url));
    TEST_ASSERT_EQUAL_size_t(1 + 3 * 1300, strlen(u.target));

    static char host[300];
    memcpy(host, "http://", 7);
    memset(host + 7, 'a', XC_URL_MAX_HOST);
    host[7 + XC_URL_MAX_HOST] = '\0';
    TEST_ASSERT_EQUAL_INT(XC_OK, parse(host));
    host[7 + XC_URL_MAX_HOST] = 'a';
    host[8 + XC_URL_MAX_HOST] = '\0';
    TEST_ASSERT_EQUAL_INT(XC_ERR_URL, parse(host));
}

static xc_url base;
static xc_url out;

static void resolve(const char *b, const char *ref, const char *want)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(XC_OK, xc_url_parse(b, strlen(b), &base), b);
    TEST_ASSERT_EQUAL_INT_MESSAGE(XC_OK, xc_url_resolve(&base, ref, strlen(ref), &out), ref);
    char got[XC_URL_MAX_TARGET + 300];
    (void)snprintf(got, sizeof got, "%s://%s:%u%s", out.scheme == XC_URL_HTTPS ? "https" : "http",
                   out.host, (unsigned)out.port, out.target);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want, got, ref);
}

/* RFC 3986 §5.4 examples, adapted to http, plus the forms servers really send. */
static void test_resolve(void)
{
    const char *b = "http://a/b/c/d;p?q";
    resolve(b, "g", "http://a:80/b/c/g");
    resolve(b, "./g", "http://a:80/b/c/g");
    resolve(b, "g/", "http://a:80/b/c/g/");
    resolve(b, "/g", "http://a:80/g");
    resolve(b, "//g", "http://g:80/");
    resolve(b, "?y", "http://a:80/b/c/d;p?y");
    resolve(b, "g?y", "http://a:80/b/c/g?y");
    resolve(b, "g#s", "http://a:80/b/c/g");
    resolve(b, ";x", "http://a:80/b/c/;x");
    resolve(b, ".", "http://a:80/b/c/");
    resolve(b, "..", "http://a:80/b/");
    resolve(b, "../g", "http://a:80/b/g");
    resolve(b, "../..", "http://a:80/");
    resolve(b, "../../../g", "http://a:80/g");
    resolve(b, "/./g", "http://a:80/g");
    resolve(b, "g.", "http://a:80/b/c/g.");
    resolve(b, "..g", "http://a:80/b/c/..g");
    resolve(b, "./../g", "http://a:80/b/g");
    resolve(b, "g;x=1/../y", "http://a:80/b/c/y");

    resolve("http://archive.example/download/x/y.iso",
            "https://dn1.ca.archive.example/0/items/x/y.iso",
            "https://dn1.ca.archive.example:443/0/items/x/y.iso");
    resolve("https://h:8443/a", "//other/b c", "https://other:443/b%20c");
    resolve("http://h:81/dir/f", "/new path/f", "http://h:81/new%20path/f");
    resolve("http://h/a%20b/f", "g", "http://h:80/a%20b/g"); /* encoding is idempotent */
}

static void test_resolve_rejected(void)
{
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_url_parse("http://h/a", 10, &base));
    static const char *const bad[] = {"",           "#only",       "ftp://x/y",
                                      "mailto:a@b", "http://u@h/", "/a\r\nb",
                                      "//[::1]/",   "javascript:x"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(XC_ERR_URL,
                                      xc_url_resolve(&base, bad[i], strlen(bad[i]), &out), bad[i]);
    }
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_url_resolve(&base, "x", 1, &base));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_basic);
    RUN_TEST(test_percent_encoding);
    RUN_TEST(test_dot_segments);
    RUN_TEST(test_rejected);
    RUN_TEST(test_limits);
    RUN_TEST(test_resolve);
    RUN_TEST(test_resolve_rejected);
    return UNITY_END();
}
