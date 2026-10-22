/* Tests for the repeat of held D-pad directions (app/ui/input.h, input.c). */
#include "input.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_only_directions_repeat(void)
{
    TEST_ASSERT_TRUE(ui_button_repeats(UI_BTN_UP));
    TEST_ASSERT_TRUE(ui_button_repeats(UI_BTN_DOWN));
    TEST_ASSERT_TRUE(ui_button_repeats(UI_BTN_LEFT));
    TEST_ASSERT_TRUE(ui_button_repeats(UI_BTN_RIGHT));
    for (int b = UI_BTN_A; b < UI_BTN_COUNT_; b++) {
        TEST_ASSERT_FALSE(ui_button_repeats((ui_button)b));
    }

    ui_repeat r = {0};
    ui_repeat_down(&r, UI_BTN_A, 1000);
    TEST_ASSERT_EQUAL_INT(UI_BTN_NONE, ui_repeat_due(&r, 5000));
}

static void test_delay_then_interval(void)
{
    ui_repeat r = {0};
    ui_repeat_down(&r, UI_BTN_DOWN, 1000);
    TEST_ASSERT_EQUAL_INT(UI_BTN_NONE, ui_repeat_due(&r, 1000));
    TEST_ASSERT_EQUAL_INT(UI_BTN_NONE, ui_repeat_due(&r, 1000 + UI_REPEAT_DELAY_MS - 1));
    TEST_ASSERT_EQUAL_INT(UI_BTN_DOWN, ui_repeat_due(&r, 1000 + UI_REPEAT_DELAY_MS));
    /* One press per poll, then the next one an interval later. */
    TEST_ASSERT_EQUAL_INT(UI_BTN_NONE, ui_repeat_due(&r, 1000 + UI_REPEAT_DELAY_MS));
    TEST_ASSERT_EQUAL_INT(UI_BTN_DOWN,
                          ui_repeat_due(&r, 1000 + UI_REPEAT_DELAY_MS + UI_REPEAT_EVERY_MS));
}

static void test_release_and_replace(void)
{
    ui_repeat r = {0};
    ui_repeat_down(&r, UI_BTN_UP, 0);
    ui_repeat_down(&r, UI_BTN_LEFT, 100); /* the newer direction wins, with a new delay */
    TEST_ASSERT_EQUAL_INT(UI_BTN_NONE, ui_repeat_due(&r, UI_REPEAT_DELAY_MS));
    TEST_ASSERT_EQUAL_INT(UI_BTN_LEFT, ui_repeat_due(&r, 100 + UI_REPEAT_DELAY_MS));
    ui_repeat_up(&r, UI_BTN_UP); /* not the one held */
    TEST_ASSERT_EQUAL_INT(UI_BTN_LEFT, ui_repeat_due(&r, 2000));
    ui_repeat_up(&r, UI_BTN_LEFT);
    TEST_ASSERT_EQUAL_INT(UI_BTN_NONE, ui_repeat_due(&r, 5000));
}

static void test_time_wraps(void)
{
    ui_repeat r = {0};
    uint32_t start = 0xFFFFFF00u; /* 256 ms before the tick count wraps */
    ui_repeat_down(&r, UI_BTN_RIGHT, start);
    TEST_ASSERT_EQUAL_INT(UI_BTN_NONE, ui_repeat_due(&r, 0xFFFFFFF0u));
    TEST_ASSERT_EQUAL_INT(UI_BTN_NONE, ui_repeat_due(&r, 10));
    TEST_ASSERT_EQUAL_INT(UI_BTN_RIGHT, ui_repeat_due(&r, start + UI_REPEAT_DELAY_MS));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_only_directions_repeat);
    RUN_TEST(test_delay_then_interval);
    RUN_TEST(test_release_and_replace);
    RUN_TEST(test_time_wraps);
    return UNITY_END();
}
