/* Tests for the UI script (xcat/ui_script.h, docs/formats/ui-script.md). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "xcat/ui_script.h"

void setUp(void) {}
void tearDown(void) {}

static xc_ui_script script; /* 60 KB: not on the stack */

static xc_err parse(const char *text, xc_ui_script_error *err)
{
    return xc_ui_script_parse(text, strlen(text), &script, err);
}

static void test_ok(void)
{
    xc_ui_script_error err;
    TEST_ASSERT_EQUAL_INT(XC_OK, parse("# xcat-ui-script v1\n", &err));
    TEST_ASSERT_EQUAL_size_t(0, script.count);

    TEST_ASSERT_EQUAL_INT(XC_OK, parse("\xEF\xBB\xBF\r\n  # xcat-ui-script v1.2\r\n"
                                       "# a comment\r\n\r\n"
                                       "  expect games \r\n"
                                       "press DOWN 3\r\n"
                                       "Press a\r\n"
                                       "wait 0\r\n"
                                       "expect install-ok 3600000\r\n"
                                       "note  Settings: \xC3\xA9 # kept  \r\n"
                                       "press white",
                                       &err));
    TEST_ASSERT_EQUAL_size_t(7, script.count);
    const xc_ui_step *st = script.steps;
    TEST_ASSERT_EQUAL_INT(XC_UI_EXPECT, st[0].op);
    TEST_ASSERT_EQUAL_STRING("games", st[0].text);
    TEST_ASSERT_EQUAL_UINT32(XC_UI_SCRIPT_EXPECT_MS, st[0].ms); /* §4.3's default */
    TEST_ASSERT_EQUAL_size_t(5, st[0].line);
    TEST_ASSERT_EQUAL_INT(XC_UI_PRESS, st[1].op);
    TEST_ASSERT_EQUAL_INT(XC_UI_KEY_DOWN, st[1].key);
    TEST_ASSERT_EQUAL_UINT32(3, st[1].count);
    TEST_ASSERT_EQUAL_INT(XC_UI_KEY_A, st[2].key);
    TEST_ASSERT_EQUAL_UINT32(1, st[2].count);
    TEST_ASSERT_EQUAL_INT(XC_UI_WAIT, st[3].op);
    TEST_ASSERT_EQUAL_UINT32(0, st[3].ms);
    TEST_ASSERT_EQUAL_STRING("install-ok", st[4].text);
    TEST_ASSERT_EQUAL_UINT32(3600000, st[4].ms);
    TEST_ASSERT_EQUAL_INT(XC_UI_NOTE, st[5].op);
    TEST_ASSERT_EQUAL_STRING("Settings: \xC3\xA9 # kept", st[5].text);
    TEST_ASSERT_EQUAL_INT(XC_UI_KEY_WHITE, st[6].key);
    TEST_ASSERT_EQUAL_size_t(11, st[6].line);

    for (int k = 0; k < XC_UI_KEY_COUNT_; k++) {
        char line[64];
        (void)snprintf(line, sizeof line, "# xcat-ui-script v1\npress %s\n",
                       xc_ui_key_name((xc_ui_key)k));
        TEST_ASSERT_EQUAL_INT_MESSAGE(XC_OK, parse(line, &err), line);
        TEST_ASSERT_EQUAL_INT(k, script.steps[0].key);
    }
    TEST_ASSERT_EQUAL_STRING("?", xc_ui_key_name(XC_UI_KEY_COUNT_));
}

static void test_errors(void)
{
    static const struct {
        const char *text;
        size_t line;
        const char *message;
    } cases[] = {
        {"", 0, "empty file (missing \"# xcat-ui-script v1\")"},
        {"press a\n", 1, "not a UI script (missing \"# xcat-ui-script v1\")"},
        {"# xcat-ui-script v2\n", 1, "needs a newer xiso-cartographer (UI script format v2)"},
        {"# xcat-ui-script v1\npush a\n", 2, "unknown command \"push\""},
        {"# xcat-ui-script v1\npress\n", 2, "press needs a button"},
        {"# xcat-ui-script v1\npress z\n", 2, "unknown button \"z\""},
        {"# xcat-ui-script v1\npress a 0\n", 2, "a press's count must be 1 to 100"},
        {"# xcat-ui-script v1\npress a 101\n", 2, "a press's count must be 1 to 100"},
        {"# xcat-ui-script v1\npress a 01\n", 2, "a press's count must be 1 to 100"},
        {"# xcat-ui-script v1\npress a 2 3\n", 2, "press has too many words"},
        {"# xcat-ui-script v1\nwait\n", 2, "wait needs a time of 0 to 600000 ms"},
        {"# xcat-ui-script v1\nwait -1\n", 2, "wait needs a time of 0 to 600000 ms"},
        {"# xcat-ui-script v1\nwait 600001\n", 2, "wait needs a time of 0 to 600000 ms"},
        {"# xcat-ui-script v1\nwait 1 2\n", 2, "wait has too many words"},
        {"# xcat-ui-script v1\nexpect\n", 2,
         "expect needs a screen's name: 1 to 31 of a-z, 0-9 and -"},
        {"# xcat-ui-script v1\nexpect Games\n", 2,
         "expect needs a screen's name: 1 to 31 of a-z, 0-9 and -"},
        {"# xcat-ui-script v1\nexpect abcdefghijabcdefghijabcdefghijab\n", 2,
         "expect needs a screen's name: 1 to 31 of a-z, 0-9 and -"},
        {"# xcat-ui-script v1\nexpect games 0\n", 2, "expect's time must be 1 to 3600000 ms"},
        {"# xcat-ui-script v1\nnote\n", 2, "note has no text"},
        {"# xcat-ui-script v1\nnote a\x01b\n", 2, "a line has a control character"},
        {"# xcat-ui-script v1\nnote \xC3\n", 2, "not UTF-8 text"},
    };
    xc_ui_script_error err;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(XC_ERR_UI_SCRIPT, parse(cases[i].text, &err), cases[i].text);
        TEST_ASSERT_EQUAL_size_t_MESSAGE(cases[i].line, err.line, cases[i].text);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(cases[i].message, err.message, cases[i].text);
        TEST_ASSERT_EQUAL_size_t(0, script.count); /* §5: nothing is played */
    }

    /* A note of exactly the most, and one byte more. */
    char note[XC_UI_SCRIPT_TEXT_SIZE + 32];
    int n = snprintf(note, sizeof note, "# xcat-ui-script v1\nnote ");
    memset(note + n, 'x', XC_UI_SCRIPT_TEXT_SIZE - 1);
    note[n + XC_UI_SCRIPT_TEXT_SIZE - 1] = '\0';
    TEST_ASSERT_EQUAL_INT(XC_OK, parse(note, &err));
    note[n + XC_UI_SCRIPT_TEXT_SIZE - 1] = 'x';
    note[n + XC_UI_SCRIPT_TEXT_SIZE] = '\0';
    TEST_ASSERT_EQUAL_INT(XC_ERR_UI_SCRIPT, parse(note, &err));
    TEST_ASSERT_EQUAL_STRING("a note is longer than 95 bytes", err.message);

    /* A NUL, a line too long, too many commands, a file too large. */
    static const char nul[] = "# xcat-ui-script v1\nnote a\0b\n";
    TEST_ASSERT_EQUAL_INT(XC_ERR_UI_SCRIPT, xc_ui_script_parse(nul, sizeof nul - 1, &script, &err));
    TEST_ASSERT_EQUAL_STRING("not UTF-8 text", err.message);

    size_t cap = XC_UI_SCRIPT_MAX_FILE + 2;
    char *big = malloc(cap);
    TEST_ASSERT_NOT_NULL(big);
    memset(big, '#', cap);
    memcpy(big, "# xcat-ui-script v1\n", 20);
    big[20 + XC_UI_SCRIPT_MAX_LINE] = '\0'; /* one line of exactly the most */
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_ui_script_parse(big, strlen(big), &script, &err));
    big[20 + XC_UI_SCRIPT_MAX_LINE] = '#';
    big[21 + XC_UI_SCRIPT_MAX_LINE] = '\0';
    TEST_ASSERT_EQUAL_INT(XC_ERR_UI_SCRIPT, xc_ui_script_parse(big, strlen(big), &script, &err));
    TEST_ASSERT_EQUAL_size_t(2, err.line);
    TEST_ASSERT_EQUAL_STRING("line longer than 256 bytes", err.message);
    TEST_ASSERT_EQUAL_INT(XC_ERR_UI_SCRIPT,
                          xc_ui_script_parse(big, XC_UI_SCRIPT_MAX_FILE + 1, &script, &err));
    TEST_ASSERT_EQUAL_STRING("file larger than 64 KiB", err.message);

    size_t len = 20;
    for (int i = 0; i <= XC_UI_SCRIPT_MAX_STEPS; i++) {
        memcpy(big + len, "wait 1\n", 7);
        len += 7;
    }
    TEST_ASSERT_EQUAL_INT(XC_ERR_UI_SCRIPT, xc_ui_script_parse(big, len, &script, &err));
    TEST_ASSERT_EQUAL_STRING("more than 512 commands", err.message);
    TEST_ASSERT_EQUAL_size_t(XC_UI_SCRIPT_MAX_STEPS + 2, err.line);
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_ui_script_parse(big, len - 7, &script, &err));
    TEST_ASSERT_EQUAL_size_t(XC_UI_SCRIPT_MAX_STEPS, script.count);
    free(big);

    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_ui_script_parse(NULL, 1, &script, &err));
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_ui_script_parse("", 0, NULL, &err));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ok);
    RUN_TEST(test_errors);
    return UNITY_END();
}
