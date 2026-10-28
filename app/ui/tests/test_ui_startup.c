/* Tests for the startup screen (app/ui/startup_view.h, startup_view.c). */
#include "fake_draw.h"
#include "startup_view.h"
#include "unity.h"

#include <stdio.h>
#include <string.h>

void setUp(void)
{
    fake_reset();
}
void tearDown(void) {}

static ui_startup_view v;

static void test_lines(void)
{
    ui_startup_open(&v, "0.8.0");
    TEST_ASSERT_EQUAL_INT(0, v.count);
    ui_startup_add(&v, "\n==== xiso-cartographer 0.8.0 ====\n");
    TEST_ASSERT_EQUAL_INT(0, v.count); /* the log's line between runs isn't shown */
    ui_startup_add(&v, "  Drives: E:, F:\r\n\n  \nGames install to F:\\Games\n");
    TEST_ASSERT_EQUAL_INT(2, v.count);
    TEST_ASSERT_EQUAL_STRING("Drives: E:, F:", v.lines[0]);
    TEST_ASSERT_EQUAL_STRING("Games install to F:\\Games", v.lines[1]);
    ui_startup_add(&v, "");
    ui_startup_add(&v, "   ");
    TEST_ASSERT_EQUAL_INT(2, v.count);

    /* The oldest go when there are more than fit. */
    char line[32];
    for (int i = 0; i < UI_STARTUP_LINES; i++) {
        (void)snprintf(line, sizeof line, "line %d", i);
        ui_startup_add(&v, line);
    }
    TEST_ASSERT_EQUAL_INT(UI_STARTUP_LINES, v.count);
    TEST_ASSERT_EQUAL_STRING("line 0", v.lines[0]);
    (void)snprintf(line, sizeof line, "line %d", UI_STARTUP_LINES - 1);
    TEST_ASSERT_EQUAL_STRING(line, v.lines[UI_STARTUP_LINES - 1]);
}

static void test_long_line_cut_between_code_points(void)
{
    ui_startup_open(&v, "0.8.0");
    char s[UI_STARTUP_LINE_MAX + 8];
    memset(s, 'a', sizeof s);
    /* "é" (two bytes) straddling the cut. */
    s[UI_STARTUP_LINE_MAX - 2] = '\xC3';
    s[UI_STARTUP_LINE_MAX - 1] = '\xA9';
    s[sizeof s - 1] = '\0';
    ui_startup_add(&v, s);
    TEST_ASSERT_EQUAL_size_t(UI_STARTUP_LINE_MAX - 2, strlen(v.lines[0]));
}

static void test_draw(void)
{
    ui_startup_open(&v, "0.8.0");
    ui_startup_add(&v, "Older line\nNewest line\n");
    ui_startup_draw(&fake_draw, &v, 0);
    TEST_ASSERT_TRUE(fake_find_text("STARTING") >= 0);
    TEST_ASSERT_TRUE(fake_find_text("xiso-cartographer 0.8.0") >= 0);
    int older = fake_find_text("Older line");
    int newest = fake_find_text("Newest line");
    TEST_ASSERT_TRUE(older >= 0 && newest > older);
    TEST_ASSERT_TRUE(fake_log[newest].r.y > fake_log[older].r.y); /* the newest last */
    TEST_ASSERT_TRUE(fake_saw("Hold LT + RT + Back + Black"));
    TEST_ASSERT_FALSE(fake_saw("CAN'T GO ON"));
}

static void test_fail(void)
{
    ui_startup_open(&v, "0.8.0");
    ui_startup_add(&v, "Drives: E:");
    ui_startup_fail(&v, "Could not create E:\\xcat\\isos");
    TEST_ASSERT_TRUE(v.failed);
    ui_startup_draw(&fake_draw, &v, 0);
    TEST_ASSERT_TRUE(fake_find_text("STOPPED") >= 0);
    TEST_ASSERT_FALSE(fake_find_text("STARTING") >= 0);
    TEST_ASSERT_TRUE(fake_find_text("CAN'T GO ON") >= 0);
    TEST_ASSERT_TRUE(fake_saw("Could not create E:\\xcat\\isos"));
    TEST_ASSERT_TRUE(fake_find_text("Drives: E:") >= 0); /* what came before stays */
    TEST_ASSERT_TRUE(fake_saw("Hold LT + RT + Back + Black"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_lines);
    RUN_TEST(test_long_line_cut_between_code_points);
    RUN_TEST(test_draw);
    RUN_TEST(test_fail);
    return UNITY_END();
}
