/* Tests for the list, progress bar and dialog (app/ui/widgets.h, widgets.c). */
#include "fake_draw.h"
#include "theme.h"
#include "unity.h"
#include "widgets.h"

#include <stdio.h>
#include <string.h>

void setUp(void)
{
    fake_reset();
}
void tearDown(void) {}

static bool same_color(ui_color a, ui_color b)
{
    return memcmp(&a, &b, sizeof a) == 0;
}

/* The fills in the given colour, and the last of them. */
static int fills_in(ui_color c, ui_rect *last)
{
    int n = 0;
    for (int i = 0; i < fake_log_n; i++) {
        if (fake_log[i].kind == 'f' && same_color(fake_log[i].c, c)) {
            n++;
            if (last != NULL) {
                *last = fake_log[i].r;
            }
        }
    }
    return n;
}

/* ---- List */

static const char *line_text(void *ctx, int i)
{
    static char buf[32];
    (void)ctx;
    (void)snprintf(buf, sizeof buf, "line %d", i + 1);
    return buf;
}

static void test_list_rows(void)
{
    /* A line is the font's height plus a gap: 26 pixels with the fake's 20. */
    TEST_ASSERT_EQUAL_INT(26, ui_list_row_h(&fake_draw));
    TEST_ASSERT_EQUAL_INT(5, ui_list_rows(&fake_draw, (ui_rect){0, 0, 200, 130}));
    TEST_ASSERT_EQUAL_INT(5, ui_list_rows(&fake_draw, (ui_rect){0, 0, 200, 155}));
    TEST_ASSERT_EQUAL_INT(1, ui_list_rows(&fake_draw, (ui_rect){0, 0, 200, 10}));
}

static void test_list_moves_pages_and_stops(void)
{
    ui_list l = {12, 0, 0};
    TEST_ASSERT_FALSE(ui_list_input(&l, UI_BTN_UP, 5)); /* already at the top */
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_TRUE(ui_list_input(&l, UI_BTN_DOWN, 5));
    }
    TEST_ASSERT_EQUAL_INT(4, l.cur);
    TEST_ASSERT_EQUAL_INT(0, l.top);
    TEST_ASSERT_TRUE(ui_list_input(&l, UI_BTN_DOWN, 5)); /* scrolls one line */
    TEST_ASSERT_EQUAL_INT(5, l.cur);
    TEST_ASSERT_EQUAL_INT(1, l.top);
    TEST_ASSERT_TRUE(ui_list_input(&l, UI_BTN_RT, 5)); /* a page */
    TEST_ASSERT_EQUAL_INT(10, l.cur);
    TEST_ASSERT_EQUAL_INT(6, l.top);
    TEST_ASSERT_TRUE(ui_list_input(&l, UI_BTN_RT, 5)); /* stops at the end */
    TEST_ASSERT_EQUAL_INT(11, l.cur);
    TEST_ASSERT_EQUAL_INT(7, l.top);
    TEST_ASSERT_FALSE(ui_list_input(&l, UI_BTN_DOWN, 5));
    TEST_ASSERT_FALSE(ui_list_input(&l, UI_BTN_A, 5)); /* the screen's */
    TEST_ASSERT_TRUE(ui_list_input(&l, UI_BTN_LT, 5));
    TEST_ASSERT_EQUAL_INT(6, l.cur);
    TEST_ASSERT_EQUAL_INT(6, l.top);
    TEST_ASSERT_TRUE(ui_list_input(&l, UI_BTN_LT, 5));
    TEST_ASSERT_TRUE(ui_list_input(&l, UI_BTN_LT, 5));
    TEST_ASSERT_EQUAL_INT(0, l.cur);
    TEST_ASSERT_EQUAL_INT(0, l.top);

    ui_list empty = {0, 0, 0};
    TEST_ASSERT_FALSE(ui_list_input(&empty, UI_BTN_DOWN, 5));
    TEST_ASSERT_EQUAL_INT(0, empty.cur);
}

static void test_list_set_count(void)
{
    ui_list l = {12, 11, 7};
    ui_list_set_count(&l, 3, 5);
    TEST_ASSERT_EQUAL_INT(2, l.cur);
    TEST_ASSERT_EQUAL_INT(0, l.top);
    ui_list_set_count(&l, 0, 5);
    TEST_ASSERT_EQUAL_INT(0, l.count);
    TEST_ASSERT_EQUAL_INT(0, l.cur);
    TEST_ASSERT_EQUAL_INT(0, l.top);
    ui_list_set_count(&l, -4, 5);
    TEST_ASSERT_EQUAL_INT(0, l.count);
}

static void test_list_draw(void)
{
    ui_list l = {12, 5, 3};
    ui_list_draw(&fake_draw, &l, (ui_rect){0, 0, 200, 130}, line_text, NULL, false);
    /* Lines 4 to 8 are shown, the 6th highlighted in bold. */
    TEST_ASSERT_EQUAL_INT(-1, fake_find_text("line 3"));
    TEST_ASSERT_EQUAL_INT(-1, fake_find_text("line 9"));
    for (int i = 4; i <= 8; i++) {
        char want[16];
        (void)snprintf(want, sizeof want, "line %d", i);
        int at = fake_find_text(want);
        TEST_ASSERT_TRUE(at >= 0);
        TEST_ASSERT_EQUAL_INT(i == 6 ? UI_FONT_BOLD : UI_FONT_BODY, fake_log[at].font);
        TEST_ASSERT_EQUAL_INT((i - 4) * 26 + 2, fake_log[at].r.y);
    }
    /* A scroll bar: the track, then the thumb, both at the right edge. */
    ui_rect thumb;
    TEST_ASSERT_TRUE(fills_in(ui_col_highlight_border, &thumb) > 0);
    TEST_ASSERT_EQUAL_INT(196, thumb.x);
    TEST_ASSERT_TRUE(thumb.y > 0 && thumb.y + thumb.h < 130);

    /* Idle: the same line highlighted, but dim. */
    fake_reset();
    ui_list_draw(&fake_draw, &l, (ui_rect){0, 0, 200, 130}, line_text, NULL, true);
    int at = fake_find_text("line 6");
    TEST_ASSERT_EQUAL_INT(UI_FONT_BOLD, fake_log[at].font);
    TEST_ASSERT_TRUE(same_color(ui_col_dim, fake_log[at].c));
    TEST_ASSERT_TRUE(same_color(ui_col_text, fake_log[fake_find_text("line 5")].c));
    TEST_ASSERT_TRUE(fills_in(ui_col_idle, NULL) > 0);
    TEST_ASSERT_EQUAL_INT(1, fills_in(ui_col_highlight_border, NULL)); /* the thumb alone */

    /* All lines fit: no scroll bar. */
    fake_reset();
    ui_list few = {3, 0, 0};
    ui_list_draw(&fake_draw, &few, (ui_rect){0, 0, 200, 130}, line_text, NULL, false);
    TEST_ASSERT_TRUE(fake_find_text("line 3") >= 0);
    int bars = 0;
    for (int i = 0; i < fake_log_n; i++) {
        if (fake_log[i].kind == 'f' && fake_log[i].r.x == 196 && fake_log[i].r.w == 4) {
            bars++;
        }
    }
    TEST_ASSERT_EQUAL_INT(0, bars);
}

/* ---- Progress bar */

static void test_percent(void)
{
    TEST_ASSERT_EQUAL_INT(0, ui_percent(0, 0));
    TEST_ASSERT_EQUAL_INT(0, ui_percent(5, 0));
    TEST_ASSERT_EQUAL_INT(0, ui_percent(0, 100));
    TEST_ASSERT_EQUAL_INT(50, ui_percent(50, 100));
    TEST_ASSERT_EQUAL_INT(99, ui_percent(999, 1000));
    TEST_ASSERT_EQUAL_INT(100, ui_percent(1000, 1000));
    TEST_ASSERT_EQUAL_INT(100, ui_percent(2000, 1000));
    /* Sizes past 4 GiB, and one byte short of the end is never 100. */
    uint64_t big = (uint64_t)1 << 40;
    TEST_ASSERT_EQUAL_INT(50, ui_percent(big / 2, big));
    TEST_ASSERT_EQUAL_INT(99, ui_percent(big - 1, big));
    TEST_ASSERT_EQUAL_INT(99, ui_percent(UINT64_MAX - 1, UINT64_MAX));
}

static void test_progress_draw(void)
{
    ui_rect r = {0, 0, 104, 30};
    ui_progress(&fake_draw, r, 1, 2, "50 %");
    /* Inside the border and a 2-pixel gap the bar is 96 pixels: half is 48. */
    ui_rect bar;
    TEST_ASSERT_TRUE(fills_in(ui_col_highlight, &bar) >= 1);
    TEST_ASSERT_EQUAL_INT(4, bar.x);
    TEST_ASSERT_EQUAL_INT(48, bar.w);
    int at = fake_find_text("50 %");
    TEST_ASSERT_TRUE(at >= 0);
    TEST_ASSERT_EQUAL_INT((104 - 40) / 2, fake_log[at].r.x);
    TEST_ASSERT_EQUAL_INT((30 - 20) / 2, fake_log[at].r.y);

    fake_reset();
    ui_progress(&fake_draw, r, 7, 0, NULL); /* nothing to show yet */
    TEST_ASSERT_EQUAL_INT(0, fills_in(ui_col_highlight, NULL));

    fake_reset();
    ui_progress(&fake_draw, r, 9, 9, NULL);
    TEST_ASSERT_TRUE(fills_in(ui_col_highlight, &bar) >= 1);
    TEST_ASSERT_EQUAL_INT(96, bar.w);
}

/* ---- Dialog */

static void test_dialog_input(void)
{
    ui_dialog dg = {"FAILED", "msg", {"RETRY", "BACK", NULL}, 0, 1};
    TEST_ASSERT_EQUAL_INT(UI_DIALOG_OPEN, ui_dialog_input(&dg, UI_BTN_LEFT));
    TEST_ASSERT_EQUAL_INT(0, dg.cur);
    TEST_ASSERT_EQUAL_INT(UI_DIALOG_OPEN, ui_dialog_input(&dg, UI_BTN_RIGHT));
    TEST_ASSERT_EQUAL_INT(UI_DIALOG_OPEN, ui_dialog_input(&dg, UI_BTN_RIGHT));
    TEST_ASSERT_EQUAL_INT(1, dg.cur); /* stops at the last choice */
    TEST_ASSERT_EQUAL_INT(1, ui_dialog_input(&dg, UI_BTN_A));
    (void)ui_dialog_input(&dg, UI_BTN_LEFT);
    TEST_ASSERT_EQUAL_INT(0, ui_dialog_input(&dg, UI_BTN_A));
    TEST_ASSERT_EQUAL_INT(1, ui_dialog_input(&dg, UI_BTN_B)); /* the cancel choice */
    TEST_ASSERT_EQUAL_INT(UI_DIALOG_OPEN, ui_dialog_input(&dg, UI_BTN_X));
    TEST_ASSERT_EQUAL_INT(UI_DIALOG_OPEN, ui_dialog_input(&dg, UI_BTN_START));

    ui_dialog must_choose = {"T", NULL, {"OK", NULL, NULL}, 0, UI_DIALOG_OPEN};
    TEST_ASSERT_EQUAL_INT(UI_DIALOG_OPEN, ui_dialog_input(&must_choose, UI_BTN_B));
    TEST_ASSERT_EQUAL_INT(0, ui_dialog_input(&must_choose, UI_BTN_A));
}

static void test_dialog_draw(void)
{
    ui_dialog dg = {"DOWNLOAD FAILED",
                    "The server closed the connection before the file was complete. Retry "
                    "to carry on where it stopped.",
                    {"RETRY", "LOG IN AGAIN", "BACK"},
                    1,
                    2};
    ui_dialog_draw(&fake_draw, &dg);
    /* The screen behind is the caller's (ui_backdrop): nothing covers it whole. */
    for (int i = 0; i < fake_log_n; i++) {
        TEST_ASSERT_FALSE(fake_log[i].kind == 'f' && fake_log[i].r.w == UI_SCREEN_W &&
                          fake_log[i].r.h == UI_SCREEN_H);
    }

    int title = fake_find_text("DOWNLOAD FAILED");
    TEST_ASSERT_TRUE(title >= 0);
    TEST_ASSERT_EQUAL_INT(UI_FONT_LABEL, fake_log[title].font);
    /* 484 pixels a line: 48 characters at most, so the message takes three lines. */
    int lines = 0;
    for (int i = 0; i < fake_log_n; i++) {
        if (fake_log[i].kind == 't' && fake_log[i].font == UI_FONT_BODY) {
            lines++;
            TEST_ASSERT_TRUE(fake_log[i].r.w <= 484);
        }
    }
    TEST_ASSERT_EQUAL_INT(3, lines);
    int retry = fake_find_text("RETRY");
    int again = fake_find_text("LOG IN AGAIN");
    int back = fake_find_text("BACK");
    TEST_ASSERT_TRUE(retry >= 0 && again >= 0 && back >= 0);
    TEST_ASSERT_TRUE(fake_log[retry].r.x < fake_log[again].r.x);
    TEST_ASSERT_TRUE(fake_log[again].r.x < fake_log[back].r.x);
    TEST_ASSERT_TRUE(same_color(ui_col_text, fake_log[again].c)); /* highlighted */
    TEST_ASSERT_TRUE(same_color(ui_col_dim, fake_log[retry].c));
    /* Everything inside the screen. */
    for (int i = 0; i < fake_log_n; i++) {
        if (fake_log[i].kind == 't') {
            TEST_ASSERT_TRUE(fake_log[i].r.x >= 0 && fake_log[i].r.x + fake_log[i].r.w <= 640);
            TEST_ASSERT_TRUE(fake_log[i].r.y >= 0 && fake_log[i].r.y + FAKE_LINE_H <= 480);
        }
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_list_rows);
    RUN_TEST(test_list_moves_pages_and_stops);
    RUN_TEST(test_list_set_count);
    RUN_TEST(test_list_draw);
    RUN_TEST(test_percent);
    RUN_TEST(test_progress_draw);
    RUN_TEST(test_dialog_input);
    RUN_TEST(test_dialog_draw);
    return UNITY_END();
}
