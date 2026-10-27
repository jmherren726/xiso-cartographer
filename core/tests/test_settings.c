/* Tests for the settings file (xcat/settings.h, docs/formats/settings.md). */
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "xcat/settings.h"

void setUp(void) {}
void tearDown(void) {}

static xc_err parse(const char *text, xc_settings *s, xc_settings_error *err)
{
    return xc_settings_parse(text, strlen(text), s, err);
}

static void test_ok(void)
{
    xc_settings s;
    xc_settings_error err;
    TEST_ASSERT_EQUAL_INT(XC_OK, parse("# xcat-settings v1\n", &s, &err));
    TEST_ASSERT_EQUAL_STRING("E:\\Games", s.install_dir); /* §4.1's default */

    TEST_ASSERT_EQUAL_INT(XC_OK, parse("\xEF\xBB\xBF\r\n  # xcat-settings v1.3\r\n"
                                       "# a comment\r\n\r\n"
                                       "  install_dir\t=  f:\\Games\\Xbox  \r\n"
                                       "newer_key = whatever # it is\r\n",
                                       &s, &err));
    TEST_ASSERT_EQUAL_STRING("F:\\Games\\Xbox", s.install_dir); /* the drive in capitals */

    /* No line ending at the end. */
    TEST_ASSERT_EQUAL_INT(XC_OK, parse("# xcat-settings v1\ninstall_dir=G:\\G", &s, &err));
    TEST_ASSERT_EQUAL_STRING("G:\\G", s.install_dir);
}

static void test_errors(void)
{
    static const struct {
        const char *text;
        size_t line;
        const char *message;
    } cases[] = {
        {"", 0, "empty file (missing \"# xcat-settings v1\")"},
        {"\n  \n", 0, "empty file (missing \"# xcat-settings v1\")"},
        {"install_dir = E:\\Games\n", 1, "not a settings file (missing \"# xcat-settings v1\")"},
        {"# xcat-settings v2\n", 1, "needs a newer xiso-cartographer (settings format v2)"},
        {"# xcat-settings v0\n", 1, "unsupported settings format v0"},
        {"# xcat-settings v1\n\ninstall_dir\n", 3,
         "a line is neither a comment nor \"key = value\""},
        {"# xcat-settings v1\nInstall_Dir = E:\\Games\n", 2, "a key name may only have a-z and _"},
        {"# xcat-settings v1\n= E:\\Games\n", 2, "a key name may only have a-z and _"},
        {"# xcat-settings v1\ninstall_dir =\n", 2, "install_dir has no value"},
        {"# xcat-settings v1\ninstall_dir = E:\\A\ninstall_dir = E:\\B\n", 3,
         "install_dir is given twice"},
        {"# xcat-settings v1\ninstall_dir = C:\\Games\n", 2,
         "install_dir must start with E:\\, F:\\ or G:\\"},
        {"# xcat-settings v1\nx = a\x01b\n", 2, "a line has a control character"},
        {"# xcat-settings v1\nx = \xC3\n", 2, "not UTF-8 text"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        xc_settings s;
        xc_settings_error err;
        TEST_ASSERT_EQUAL_INT_MESSAGE(XC_ERR_SETTINGS, parse(cases[i].text, &s, &err),
                                      cases[i].text);
        TEST_ASSERT_EQUAL_size_t_MESSAGE(cases[i].line, err.line, cases[i].text);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(cases[i].message, err.message, cases[i].text);
        TEST_ASSERT_EQUAL_STRING("E:\\Games", s.install_dir); /* §5: the defaults */
    }

    /* A NUL, a line too long, a file too large. */
    xc_settings s;
    xc_settings_error err;
    static const char nul[] = "# xcat-settings v1\nx = a\0b\n";
    TEST_ASSERT_EQUAL_INT(XC_ERR_SETTINGS, xc_settings_parse(nul, sizeof nul - 1, &s, &err));
    TEST_ASSERT_EQUAL_STRING("not UTF-8 text", err.message);
    char *big = malloc(XC_SETTINGS_MAX_FILE + 2);
    TEST_ASSERT_NOT_NULL(big);
    memset(big, '#', XC_SETTINGS_MAX_FILE + 1);
    memcpy(big, "# xcat-settings v1\n", 19);
    big[19 + XC_SETTINGS_MAX_LINE] = '\0'; /* one line of exactly the most */
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_settings_parse(big, strlen(big), &s, &err));
    big[19 + XC_SETTINGS_MAX_LINE] = '#';
    big[20 + XC_SETTINGS_MAX_LINE] = '\0';
    TEST_ASSERT_EQUAL_INT(XC_ERR_SETTINGS, xc_settings_parse(big, strlen(big), &s, &err));
    TEST_ASSERT_EQUAL_size_t(2, err.line);
    TEST_ASSERT_EQUAL_STRING("line longer than 1024 bytes", err.message);
    TEST_ASSERT_EQUAL_INT(XC_ERR_SETTINGS,
                          xc_settings_parse(big, XC_SETTINGS_MAX_FILE + 1, &s, &err));
    TEST_ASSERT_EQUAL_STRING("file larger than 16 KiB", err.message);
    free(big);
}

static void test_check_dir(void)
{
    TEST_ASSERT_NULL(xc_settings_check_dir("E:\\Games"));
    TEST_ASSERT_NULL(xc_settings_check_dir("f:\\Games"));
    TEST_ASSERT_NULL(xc_settings_check_dir("G:\\Xbox games\\Installed (2026)"));
    /* 64 bytes after the drive, the most. */
    TEST_ASSERT_NULL(xc_settings_check_dir(
        "E:\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghi"));
    TEST_ASSERT_EQUAL_STRING(
        "is longer than 64 characters after the drive",
        xc_settings_check_dir(
            "E:\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghij"));
    TEST_ASSERT_EQUAL_STRING("must start with E:\\, F:\\ or G:\\",
                             xc_settings_check_dir("C:\\Games"));
    TEST_ASSERT_EQUAL_STRING("must start with E:\\, F:\\ or G:\\",
                             xc_settings_check_dir("E:Games"));
    TEST_ASSERT_EQUAL_STRING("must start with E:\\, F:\\ or G:\\", xc_settings_check_dir("Games"));
    TEST_ASSERT_EQUAL_STRING("must start with E:\\, F:\\ or G:\\", xc_settings_check_dir(""));
    TEST_ASSERT_EQUAL_STRING("is a whole drive: name a folder on it, like E:\\Games",
                             xc_settings_check_dir("E:\\"));
    TEST_ASSERT_EQUAL_STRING("has an empty folder name (\\\\, or a \\ at the end)",
                             xc_settings_check_dir("E:\\Games\\"));
    TEST_ASSERT_EQUAL_STRING("has an empty folder name (\\\\, or a \\ at the end)",
                             xc_settings_check_dir("E:\\\\Games"));
    TEST_ASSERT_EQUAL_STRING(
        "has a folder name longer than 42 characters",
        xc_settings_check_dir("E:\\abcdefghijklmnopqrstuvwxyzabcdefghijklmnopq"));
    /* Found by fuzz_settings: the file trims the end of a line, so it would read back as
       another folder. */
    TEST_ASSERT_EQUAL_STRING("has a folder name that ends with a space",
                             xc_settings_check_dir("E:\\s#G "));
    TEST_ASSERT_EQUAL_STRING("has a folder name that ends with a space",
                             xc_settings_check_dir("E:\\Xbox \\Games"));
    TEST_ASSERT_NULL(xc_settings_check_dir("E:\\ Games"));
    TEST_ASSERT_EQUAL_STRING("can't use . or .. as a folder name",
                             xc_settings_check_dir("E:\\Games\\.."));
    TEST_ASSERT_EQUAL_STRING("has a character the Xbox drive can't store",
                             xc_settings_check_dir("E:\\Ga*mes"));
    TEST_ASSERT_EQUAL_STRING("has a character the Xbox drive can't store",
                             xc_settings_check_dir("E:\\Games/Xbox"));
    TEST_ASSERT_EQUAL_STRING("is missing", xc_settings_check_dir(NULL));
}

static void set_ok(const char *before, const char *value, const char *after)
{
    char *out = NULL;
    size_t len = 0;
    xc_settings_error err;
    TEST_ASSERT_EQUAL_INT_MESSAGE(XC_OK,
                                  xc_settings_set(before, before != NULL ? strlen(before) : 0,
                                                  "install_dir", value, &out, &len, &err),
                                  err.message);
    TEST_ASSERT_EQUAL_STRING(after, out);
    TEST_ASSERT_EQUAL_size_t(strlen(after), len);
    free(out);
}

static void test_set(void)
{
    /* §6: a new file; the key's line replaced in place; or added at the end. */
    set_ok(NULL, "F:\\Games", "# xcat-settings v1\n\ninstall_dir = F:\\Games\n");
    set_ok("# xcat-settings v1\r\n# mine\r\ninstall_dir=E:\\A\r\nother = 1\r\n", "G:\\B",
           "# xcat-settings v1\r\n# mine\r\ninstall_dir = G:\\B\nother = 1\r\n");
    set_ok("# xcat-settings v1\ninstall_dir = E:\\A", "E:\\B",
           "# xcat-settings v1\ninstall_dir = E:\\B\n");
    set_ok("# xcat-settings v1\n# install_dir = E:\\Old\nother = 1", "E:\\B",
           "# xcat-settings v1\n# install_dir = E:\\Old\nother = 1\ninstall_dir = E:\\B\n");

    char *out = NULL;
    size_t len = 0;
    xc_settings_error err;
    /* A file that doesn't parse is left alone; so is a value that wouldn't. */
    static const char bad[] = "# xcat-settings v1\ninstall_dir = C:\\Games\n";
    TEST_ASSERT_EQUAL_INT(XC_ERR_SETTINGS, xc_settings_set(bad, sizeof bad - 1, "install_dir",
                                                           "E:\\Games", &out, &len, &err));
    TEST_ASSERT_EQUAL_size_t(2, err.line);
    TEST_ASSERT_NULL(out);
    TEST_ASSERT_EQUAL_INT(XC_ERR_SETTINGS,
                          xc_settings_set(NULL, 0, "install_dir", "E:\\", &out, &len, &err));
    TEST_ASSERT_NULL(out);
    /* A line break would write a second line. */
    TEST_ASSERT_EQUAL_INT(
        XC_ERR_INVALID_ARG,
        xc_settings_set(NULL, 0, "install_dir", "E:\\A\ninstall_dir = E:\\B", &out, &len, &err));
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG,
                          xc_settings_set(NULL, 0, "Bad key", "1", &out, &len, &err));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ok);
    RUN_TEST(test_errors);
    RUN_TEST(test_check_dir);
    RUN_TEST(test_set);
    return UNITY_END();
}
