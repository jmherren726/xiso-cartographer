/* Tests for the strict JSON pull parser (json.h, docs/formats/archiveorg.md §6). */
#include "json.h"
#include "unity.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* Reads the whole text; returns the first error, or XC_OK at XC_JSON_END. */
static xc_err walk(const char *s, size_t *tokens)
{
    xc_json j;
    xc_json_init(&j, s, strlen(s));
    size_t n = 0;
    for (;;) {
        xc_json_tok t;
        xc_err err = xc_json_next(&j, &t);
        if (err != XC_OK) {
            return err;
        }
        if (t.type == XC_JSON_END) {
            break;
        }
        n++;
    }
    if (tokens != NULL) {
        *tokens = n;
    }
    return XC_OK;
}

static void test_valid(void)
{
    static const char *const ok[] = {
        "{}",
        "[]",
        " \t\r\n{ } \n",
        "0",
        "-0",
        "-12.5e+3",
        "1E9",
        "\"\"",
        "true",
        "false",
        "null",
        "{\"a\":[1,{\"b\":null},\"x\"],\"c\":{}}",
        "[[[[]]]]",
        "\"\\\"\\\\\\/\\b\\f\\n\\r\\t\\u00e9\"",
        "\"caf\xc3\xa9\"",
    };
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(XC_OK, walk(ok[i], NULL), ok[i]);
    }
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(XC_OK, walk("{\"a\":[1,{\"b\":null},\"x\"],\"c\":{}}", &n));
    /* { a [ 1 { b null } x ] c { } } */
    TEST_ASSERT_EQUAL_size_t(14, n);
}

static void test_invalid(void)
{
    static const char *const bad[] = {
        "",
        "   ",
        "{",
        "}",
        "[1,]",
        "{\"a\":1,}",
        "{\"a\" 1}",
        "{a:1}",
        "{\"a\":1 \"b\":2}",
        "[1 2]",
        "{} {}",
        "01",
        "1.",
        ".5",
        "+1",
        "1e",
        "-",
        "NaN",
        "tru",
        "nul",
        "'x'",
        "\"abc",
        "\"a\nb\"",
        "\"\\x\"",
        "\"\\u12\"",
        "\"\\u12g4\"",
        "\"\xc3\"",
        "\"\xed\xa0\x80\"",
        "[1]]",
        "{\"a\":1}}",
        "[}",
        "{]",
        "/* c */ 1",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(XC_ERR_CORRUPT, walk(bad[i], NULL), bad[i]);
    }
}

static void test_depth(void)
{
    char deep[2 * (XC_JSON_MAX_DEPTH + 1) + 1];
    for (size_t d = XC_JSON_MAX_DEPTH; d <= XC_JSON_MAX_DEPTH + 1; d++) {
        memset(deep, '[', d);
        memset(deep + d, ']', d);
        deep[2 * d] = '\0';
        TEST_ASSERT_EQUAL_INT(d == XC_JSON_MAX_DEPTH ? XC_OK : XC_ERR_LIMIT, walk(deep, NULL));
    }
    /* Mixed objects and arrays at the limit keep track of which is which. */
    char mixed[8 * XC_JSON_MAX_DEPTH];
    size_t n = 0;
    for (int d = 0; d < XC_JSON_MAX_DEPTH; d++) {
        const char *open = d % 2 == 0 ? "{\"k\":" : "[";
        memcpy(mixed + n, open, strlen(open));
        n += strlen(open);
    }
    for (int d = XC_JSON_MAX_DEPTH - 1; d >= 0; d--) {
        mixed[n++] = d % 2 == 0 ? '}' : ']';
    }
    mixed[n] = '\0';
    TEST_ASSERT_EQUAL_INT(XC_OK, walk(mixed, NULL));
}

static void test_strings(void)
{
    const char *s = "[\"a\\u00e9\\ud83d\\ude00\\n\", \"plain\", \"\\ud83d\", \"\\ude00\", "
                    "\"\\u0000\", \"\\ud83dx\", \"long string\"]";
    xc_json j;
    xc_json_init(&j, s, strlen(s));
    xc_json_tok t;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_json_next(&j, &t));
    TEST_ASSERT_EQUAL_INT(XC_JSON_ARRAY, t.type);
    char buf[32];
    size_t len = 0;

    TEST_ASSERT_EQUAL_INT(XC_OK, xc_json_next(&j, &t));
    TEST_ASSERT_TRUE(t.escaped);
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_json_string(&t, buf, sizeof buf, &len));
    TEST_ASSERT_EQUAL_STRING("a\xc3\xa9\xf0\x9f\x98\x80\n", buf);
    TEST_ASSERT_EQUAL_size_t(8, len);

    TEST_ASSERT_EQUAL_INT(XC_OK, xc_json_next(&j, &t));
    TEST_ASSERT_FALSE(t.escaped);
    TEST_ASSERT_TRUE(xc_json_equals(&t, "plain"));
    TEST_ASSERT_FALSE(xc_json_equals(&t, "plai"));
    TEST_ASSERT_EQUAL_INT(XC_ERR_LIMIT, xc_json_string(&t, buf, 5, NULL));
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_json_string(&t, buf, 6, NULL));

    for (int i = 0; i < 4; i++) { /* lone high, lone low, NUL, high + other */
        TEST_ASSERT_EQUAL_INT(XC_OK, xc_json_next(&j, &t));
        TEST_ASSERT_EQUAL_INT(XC_ERR_CORRUPT, xc_json_string(&t, buf, sizeof buf, NULL));
        TEST_ASSERT_FALSE(xc_json_equals(&t, ""));
    }
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_json_next(&j, &t));
    TEST_ASSERT_EQUAL_INT(XC_ERR_LIMIT, xc_json_string(&t, buf, 4, NULL));

    /* An escaped key compares by its decoded text. */
    const char *k = "{\"n\\u0061me\":1}";
    xc_json_init(&j, k, strlen(k));
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_json_next(&j, &t));
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_json_next(&j, &t));
    TEST_ASSERT_EQUAL_INT(XC_JSON_KEY, t.type);
    TEST_ASSERT_TRUE(xc_json_equals(&t, "name"));
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_json_string(&t, buf, 0, NULL));
}

static void test_skip(void)
{
    const char *s = "{\"skip\":{\"a\":[1,[2,{\"b\":3}]],\"c\":\"}\"},\"keep\":true}";
    xc_json j;
    xc_json_init(&j, s, strlen(s));
    xc_json_tok t;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_json_next(&j, &t));
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_json_next(&j, &t));
    TEST_ASSERT_TRUE(xc_json_equals(&t, "skip"));
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_json_next(&j, &t));
    TEST_ASSERT_EQUAL_INT(XC_JSON_OBJECT, t.type);
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_json_skip(&j, &t));
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_json_next(&j, &t));
    TEST_ASSERT_TRUE(xc_json_equals(&t, "keep"));
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_json_next(&j, &t));
    TEST_ASSERT_EQUAL_INT(XC_JSON_TRUE, t.type);
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_json_skip(&j, &t)); /* a scalar: nothing to do */
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_json_next(&j, &t));
    TEST_ASSERT_EQUAL_INT(XC_JSON_OBJECT_END, t.type);
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_json_next(&j, &t));
    TEST_ASSERT_EQUAL_INT(XC_JSON_END, t.type);

    /* Errors inside a skipped value are reported, and stick. */
    const char *bad = "[{\"a\":[1,}]]";
    xc_json_init(&j, bad, strlen(bad));
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_json_next(&j, &t));
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_json_next(&j, &t));
    TEST_ASSERT_EQUAL_INT(XC_ERR_CORRUPT, xc_json_skip(&j, &t));
    TEST_ASSERT_EQUAL_INT(XC_ERR_CORRUPT, xc_json_next(&j, &t));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_valid);
    RUN_TEST(test_invalid);
    RUN_TEST(test_depth);
    RUN_TEST(test_strings);
    RUN_TEST(test_skip);
    return UNITY_END();
}
