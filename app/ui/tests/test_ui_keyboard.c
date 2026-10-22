/* Tests for the on-screen keyboard (app/ui/keyboard.h, keyboard.c). */
#include "fake_draw.h"
#include "keyboard.h"
#include "unity.h"

#include <string.h>

void setUp(void)
{
    fake_reset();
}
void tearDown(void) {}

static char buf[32];
static ui_kbd k;

static void open_empty(bool password)
{
    memset(buf, 0, sizeof buf);
    ui_kbd_open(&k, "FIELD", buf, sizeof buf, password);
}

static void press(ui_button b, int times)
{
    for (int i = 0; i < times; i++) {
        TEST_ASSERT_EQUAL_INT(UI_KBD_EDITING, ui_kbd_input(&k, b));
    }
}

static void test_open_keeps_text(void)
{
    strcpy(buf, "abc");
    ui_kbd_open(&k, "SEARCH", buf, sizeof buf, false);
    TEST_ASSERT_EQUAL_STRING("abc", buf);
    TEST_ASSERT_EQUAL_INT(UI_KBD_LOWER, k.layer);
    TEST_ASSERT_EQUAL_INT(1, k.row);
    TEST_ASSERT_EQUAL_INT(0, k.col);
    press(UI_BTN_A, 1); /* the highlight starts on "q" */
    TEST_ASSERT_EQUAL_STRING("abcq", buf);
}

static void test_type_with_the_dpad(void)
{
    open_empty(false);
    press(UI_BTN_RIGHT, 1); /* w */
    press(UI_BTN_A, 1);
    press(UI_BTN_LT, 1); /* capitals */
    press(UI_BTN_DOWN, 1);
    press(UI_BTN_A, 1); /* S */
    press(UI_BTN_LT, 1);
    press(UI_BTN_UP, 2);
    press(UI_BTN_A, 1); /* 2 */
    press(UI_BTN_Y, 1); /* space */
    press(UI_BTN_RT, 1);
    press(UI_BTN_DOWN, 1);
    press(UI_BTN_A, 1); /* ? */
    TEST_ASSERT_EQUAL_STRING("wS2 ?", buf);
    press(UI_BTN_X, 2);
    TEST_ASSERT_EQUAL_STRING("wS2", buf);
}

static void test_wraps_around(void)
{
    open_empty(false);
    press(UI_BTN_LEFT, 1); /* from "q" to "p" */
    press(UI_BTN_A, 1);
    press(UI_BTN_RIGHT, 1); /* and back */
    press(UI_BTN_A, 1);
    press(UI_BTN_UP, 2); /* past the digits to the bottom row: SHIFT */
    TEST_ASSERT_EQUAL_INT(UI_KBD_ROWS - 1, k.row);
    press(UI_BTN_DOWN, 2); /* past the bottom row to the digits, then "q" again */
    press(UI_BTN_A, 1);
    TEST_ASSERT_EQUAL_STRING("pqq", buf);
}

/* The bottom row's keys take more than one column; Left and Right go key by key. */
static void test_bottom_row(void)
{
    open_empty(false);
    k.row = UI_KBD_ROWS - 1;
    k.col = 5; /* the second column of SPACE */
    press(UI_BTN_A, 1);
    TEST_ASSERT_EQUAL_STRING(" ", buf);
    press(UI_BTN_RIGHT, 1); /* DELETE */
    press(UI_BTN_A, 1);
    TEST_ASSERT_EQUAL_STRING("", buf);
    press(UI_BTN_LEFT, 2); /* SYMBOLS */
    press(UI_BTN_A, 1);
    TEST_ASSERT_EQUAL_INT(UI_KBD_SYMBOLS, k.layer);
    press(UI_BTN_A, 1);
    TEST_ASSERT_EQUAL_INT(UI_KBD_LOWER, k.layer);
    press(UI_BTN_LEFT, 1); /* SHIFT */
    press(UI_BTN_A, 1);
    TEST_ASSERT_EQUAL_INT(UI_KBD_UPPER, k.layer);
    press(UI_BTN_LEFT, 1); /* wraps to DONE */
    TEST_ASSERT_EQUAL_INT(UI_KBD_DONE, ui_kbd_input(&k, UI_BTN_A));
}

static void test_finish_and_cancel(void)
{
    open_empty(false);
    TEST_ASSERT_EQUAL_INT(UI_KBD_DONE, ui_kbd_input(&k, UI_BTN_START));
    TEST_ASSERT_EQUAL_INT(UI_KBD_CANCEL, ui_kbd_input(&k, UI_BTN_B));
    TEST_ASSERT_EQUAL_INT(UI_KBD_EDITING, ui_kbd_input(&k, UI_BTN_BACK));
}

/* Every printable ASCII character is on some key; the space is Y. */
static void test_every_printable_character(void)
{
    bool seen[128] = {false};
    for (int layer = UI_KBD_LOWER; layer <= UI_KBD_SYMBOLS; layer++) {
        for (int row = 0; row < UI_KBD_ROWS - 1; row++) {
            for (int col = 0; col < UI_KBD_COLS; col++) {
                open_empty(false);
                k.layer = (ui_kbd_layer)layer;
                k.row = row;
                k.col = col;
                press(UI_BTN_A, 1);
                TEST_ASSERT_EQUAL_size_t(1, strlen(buf));
                seen[(unsigned char)buf[0] & 0x7F] = true;
            }
        }
    }
    for (int c = 0x21; c < 0x7F; c++) {
        TEST_ASSERT_TRUE_MESSAGE(seen[c], "a printable character no key types");
    }
}

static void test_length_limit(void)
{
    char small[4] = "";
    ui_kbd_open(&k, "PIN", small, sizeof small, false);
    press(UI_BTN_A, 5);
    TEST_ASSERT_EQUAL_STRING("qqq", small); /* 3 bytes and the NUL */
    press(UI_BTN_Y, 1);
    TEST_ASSERT_EQUAL_STRING("qqq", small);
}

static void test_delete_removes_a_code_point(void)
{
    strcpy(buf, "caf\xC3\xA9"); /* café: é is two bytes */
    ui_kbd_open(&k, "SEARCH", buf, sizeof buf, false);
    press(UI_BTN_X, 1);
    TEST_ASSERT_EQUAL_STRING("caf", buf);
    press(UI_BTN_X, 4);
    TEST_ASSERT_EQUAL_STRING("", buf);
}

static void test_draw_shows_the_text(void)
{
    strcpy(buf, "user");
    ui_kbd_open(&k, "USERNAME", buf, sizeof buf, false);
    ui_kbd_draw(&fake_draw, &k);
    TEST_ASSERT_TRUE(fake_find_text("USERNAME") >= 0);
    TEST_ASSERT_TRUE(fake_find_text("user") >= 0);
    TEST_ASSERT_TRUE(fake_find_text("q") >= 0);
    TEST_ASSERT_TRUE(fake_find_text("SHIFT") >= 0);
    TEST_ASSERT_TRUE(fake_find_text("DONE") >= 0);
}

/* A long text shows its end, so what's being typed stays in sight. */
static void test_draw_shows_the_end(void)
{
    static char long_buf[128];
    memset(long_buf, 0, sizeof long_buf);
    memset(long_buf, 'a', 60);
    long_buf[59] = 'Z';
    ui_kbd_open(&k, "SEARCH", long_buf, sizeof long_buf, false);
    ui_kbd_draw(&fake_draw, &k);
    int field = -1;
    for (int i = 0; i < fake_log_n; i++) {
        if (fake_log[i].kind == 't' && fake_log[i].font == UI_FONT_BODY) {
            field = i;
        }
    }
    TEST_ASSERT_TRUE(field >= 0);
    size_t n = strlen(fake_log[field].text);
    TEST_ASSERT_TRUE(n > 10 && n < 60);
    TEST_ASSERT_EQUAL_CHAR('Z', fake_log[field].text[n - 1]);
}

/* A password is drawn as dots, and its text never reaches the backend, not even measured. */
static void test_password_shows_dots_only(void)
{
    strcpy(buf, "hunter2");
    ui_kbd_open(&k, "PASSWORD", buf, sizeof buf, true);
    ui_kbd_draw(&fake_draw, &k);
    TEST_ASSERT_FALSE(fake_saw("hunter"));
    TEST_ASSERT_TRUE(fake_find_text("\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2"
                                    "\xE2\x80\xA2\xE2\x80\xA2") >= 0);
    TEST_ASSERT_EQUAL_STRING("hunter2", buf); /* drawing leaves the buffer alone */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_open_keeps_text);
    RUN_TEST(test_type_with_the_dpad);
    RUN_TEST(test_wraps_around);
    RUN_TEST(test_bottom_row);
    RUN_TEST(test_finish_and_cancel);
    RUN_TEST(test_every_printable_character);
    RUN_TEST(test_length_limit);
    RUN_TEST(test_delete_removes_a_code_point);
    RUN_TEST(test_draw_shows_the_text);
    RUN_TEST(test_draw_shows_the_end);
    RUN_TEST(test_password_shows_dots_only);
    return UNITY_END();
}
