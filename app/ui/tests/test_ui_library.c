/* Tests for the library screen (app/ui/library.h, library.c). */
#include "fake_draw.h"
#include "library.h"
#include "unity.h"

#include <string.h>

void setUp(void)
{
    fake_reset();
}
void tearDown(void) {}

static ui_lib_item items[3] = {
    {"First game", "2004  |  Europe  |  4.38 GB", "By someone. From the map.", UI_LIB_NOT_TRIED,
     NULL},
    {"Second game", "", "From E:/xcat/isos.", UI_LIB_INSTALLED, "In E:/Games/Second game"},
    {"Third game", "2002", NULL, UI_LIB_FAILED, "No space left"},
};

static int count_kind(char kind)
{
    int n = 0;
    for (int i = 0; i < fake_log_n; i++) {
        n += fake_log[i].kind == kind;
    }
    return n;
}

static int count_text(const char *s)
{
    int n = 0;
    for (int i = 0; i < fake_log_n; i++) {
        n += fake_log[i].kind == 't' && strcmp(fake_log[i].text, s) == 0;
    }
    return n;
}

static void test_open(void)
{
    ui_library lib;
    ui_library_open(&lib, items, 3);
    TEST_ASSERT_EQUAL_INT(0, lib.list.cur);
    TEST_ASSERT_EQUAL_INT(3, lib.list.count);
    TEST_ASSERT_EQUAL_INT(UI_LIB_GAMES, lib.section);
    TEST_ASSERT_FALSE(lib.in_sections);
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, lib.asking);
    /* 300 pixels of list at 26 a line. */
    TEST_ASSERT_EQUAL_INT(11, ui_library_rows(&fake_draw));
}

/* Nothing installs unasked: A asks first, and only INSTALL installs. */
static void test_install_asks_first(void)
{
    ui_library lib;
    ui_library_open(&lib, items, 3);
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_DOWN, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_A, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_INSTALL, lib.asking);
    TEST_ASSERT_NOT_NULL(strstr(lib.message, "Second game"));
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_B, 5)); /* CANCEL */
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, lib.asking);
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_A, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_RIGHT, 5)); /* to CANCEL */
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_A, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_A, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_INSTALL, ui_library_input(&lib, UI_BTN_A, 5));
    TEST_ASSERT_EQUAL_INT(1, lib.list.cur); /* the game asked about */
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, lib.asking);
}

static void test_back_asks_before_leaving(void)
{
    ui_library lib;
    ui_library_open(&lib, items, 3);
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_BACK, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_EXIT, lib.asking);
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_B, 5)); /* STAY */
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_BACK, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_EXIT, ui_library_input(&lib, UI_BTN_A, 5));
}

static void test_sections(void)
{
    ui_library lib;
    ui_library_open(&lib, items, 3);
    (void)ui_library_input(&lib, UI_BTN_LEFT, 5);
    TEST_ASSERT_TRUE(lib.in_sections);
    (void)ui_library_input(&lib, UI_BTN_UP, 5); /* stops at the top */
    TEST_ASSERT_EQUAL_INT(UI_LIB_GAMES, lib.section);
    for (int i = 0; i < 5; i++) {
        (void)ui_library_input(&lib, UI_BTN_DOWN, 5);
    }
    TEST_ASSERT_EQUAL_INT(UI_LIB_SETTINGS, lib.section); /* and at the bottom */
    TEST_ASSERT_EQUAL_INT(0, lib.list.cur);              /* the D-pad didn't move the list */
    (void)ui_library_input(&lib, UI_BTN_A, 5);
    TEST_ASSERT_FALSE(lib.in_sections);
    /* Not GAMES: no list to move, nothing to install. */
    (void)ui_library_input(&lib, UI_BTN_DOWN, 5);
    TEST_ASSERT_EQUAL_INT(0, lib.list.cur);
    (void)ui_library_input(&lib, UI_BTN_A, 5);
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, lib.asking);
    ui_library_draw(&fake_draw, &lib, 0, NULL);
    TEST_ASSERT_TRUE(fake_saw("Not here yet"));
    TEST_ASSERT_FALSE(fake_saw("First game"));
}

static void test_empty(void)
{
    ui_library lib;
    ui_library_open(&lib, items, 0);
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_A, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, lib.asking);
    ui_library_draw(&fake_draw, &lib, 0, "status");
    TEST_ASSERT_TRUE(fake_saw("No games"));
}

static void test_draw(void)
{
    ui_library lib;
    ui_library_open(&lib, items, 3);
    ui_library_draw(&fake_draw, &lib, 0, "Ready");
    TEST_ASSERT_TRUE(fake_find_text("LIBRARY") >= 0);
    TEST_ASSERT_TRUE(fake_find_text("GAMES") >= 0);
    TEST_ASSERT_TRUE(fake_find_text("SETTINGS") >= 0);
    TEST_ASSERT_TRUE(fake_find_text("Ready") >= 0);
    TEST_ASSERT_TRUE(fake_find_text("3 games listed") >= 0);
    TEST_ASSERT_TRUE(fake_find_text("1 installed") >= 0);
    TEST_ASSERT_TRUE(fake_find_text("1 unfinished") >= 0);
    TEST_ASSERT_TRUE(fake_saw("2004"));
    TEST_ASSERT_EQUAL_INT(1, count_text("INSTALLED")); /* the section: the game isn't tried */

    fake_reset();
    (void)ui_library_input(&lib, UI_BTN_DOWN, 5);
    ui_library_draw(&fake_draw, &lib, 0, NULL);
    TEST_ASSERT_EQUAL_INT(2, count_text("INSTALLED"));
    TEST_ASSERT_TRUE(fake_saw("Second game"));

    fake_reset();
    (void)ui_library_input(&lib, UI_BTN_DOWN, 5);
    ui_library_draw(&fake_draw, &lib, 0, NULL);
    TEST_ASSERT_TRUE(fake_find_text("FAILED") >= 0);
    TEST_ASSERT_TRUE(fake_saw("No space left"));
}

static void test_stopped(void)
{
    const ui_lib_item stopped[1] = {{"Game", "", NULL, UI_LIB_STOPPED, "Stopped."}};
    ui_library lib;
    ui_library_open(&lib, stopped, 1);
    ui_library_draw(&fake_draw, &lib, 0, NULL);
    TEST_ASSERT_TRUE(fake_find_text("STOPPED") >= 0);
    TEST_ASSERT_TRUE(fake_find_text("1 unfinished") >= 0);
}

/* Started in an earlier run: UNFINISHED, and counted as unfinished. */
static void test_unfinished(void)
{
    const ui_lib_item half[1] = {{"Game", "", NULL, UI_LIB_UNFINISHED, "In E:/Games/Game"}};
    ui_library lib;
    ui_library_open(&lib, half, 1);
    ui_library_draw(&fake_draw, &lib, 0, NULL);
    TEST_ASSERT_TRUE(fake_find_text("UNFINISHED") >= 0);
    TEST_ASSERT_TRUE(fake_find_text("1 unfinished") >= 0);
    TEST_ASSERT_TRUE(fake_find_text("0 installed") >= 0);
}

static const ui_lib_item found[2] = {
    {"Alpha folder", "1.00 GB  |  12 files", "Title 4D53000A.", UI_LIB_INSTALLED, "E:/Games/A"},
    {"Beta folder", "", "Not finished.", UI_LIB_UNFINISHED, "E:/Games/B"},
};

/* INSTALLED: its own list and place; A installs nothing there. */
static void test_installed_section(void)
{
    ui_library lib;
    ui_library_open(&lib, items, 3);
    ui_library_set_installed(&lib, found, 2);
    (void)ui_library_input(&lib, UI_BTN_LEFT, 5);
    (void)ui_library_input(&lib, UI_BTN_DOWN, 5);
    (void)ui_library_input(&lib, UI_BTN_DOWN, 5);
    TEST_ASSERT_EQUAL_INT(UI_LIB_INSTALLED_GAMES, lib.section);
    (void)ui_library_input(&lib, UI_BTN_RIGHT, 5);
    TEST_ASSERT_FALSE(lib.in_sections);
    ui_library_draw(&fake_draw, &lib, 0, NULL);
    TEST_ASSERT_TRUE(fake_saw("Alpha folder"));
    TEST_ASSERT_TRUE(fake_saw("Beta folder"));
    TEST_ASSERT_TRUE(fake_saw("12 files"));
    TEST_ASSERT_FALSE(fake_saw("First game"));
    TEST_ASSERT_FALSE(fake_saw("A install"));

    (void)ui_library_input(&lib, UI_BTN_DOWN, 5);
    TEST_ASSERT_EQUAL_INT(1, lib.installed_list.cur);
    TEST_ASSERT_EQUAL_INT(0, lib.list.cur); /* GAMES keeps its place */
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_A, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, lib.asking);
    fake_reset();
    ui_library_draw(&fake_draw, &lib, 0, NULL);
    TEST_ASSERT_TRUE(fake_find_text("UNFINISHED") >= 0);
    TEST_ASSERT_TRUE(fake_saw("Not finished."));

    /* A shorter list after an install: the place stays inside it. */
    ui_library_set_installed(&lib, found, 1);
    TEST_ASSERT_EQUAL_INT(0, lib.installed_list.cur);
    TEST_ASSERT_EQUAL_INT(1, lib.installed_list.count);
    ui_library_set_installed(&lib, found, 0);
    TEST_ASSERT_EQUAL_INT(0, lib.installed_list.cur);
    fake_reset();
    ui_library_draw(&fake_draw, &lib, 0, NULL);
    TEST_ASSERT_TRUE(fake_saw("Nothing installed yet"));
}

/* A dialog draws over the screen behind, saved in its first frame and copied after. */
static void test_dialog_over_backdrop(void)
{
    ui_library lib;
    ui_library_open(&lib, items, 3);
    (void)ui_library_input(&lib, UI_BTN_A, 5);
    ui_library_draw(&fake_draw, &lib, 0, NULL);
    TEST_ASSERT_EQUAL_INT(1, count_kind('s'));
    TEST_ASSERT_TRUE(fake_find_text("LIBRARY") >= 0); /* drawn once, then saved */
    TEST_ASSERT_TRUE(fake_find_text("INSTALL") >= 0);
    fake_reset();
    ui_library_draw(&fake_draw, &lib, 0, NULL);
    TEST_ASSERT_EQUAL_INT(0, count_kind('s'));
    TEST_ASSERT_EQUAL_INT(1, count_kind('b'));
    TEST_ASSERT_EQUAL_INT(-1, fake_find_text("LIBRARY"));
    TEST_ASSERT_TRUE(fake_find_text("CANCEL") >= 0);
    /* Closed and opened again: saved again, as the screen may have changed. */
    (void)ui_library_input(&lib, UI_BTN_B, 5);
    fake_reset();
    ui_library_draw(&fake_draw, &lib, 0, NULL);
    TEST_ASSERT_EQUAL_INT(0, count_kind('s') + count_kind('b'));
    (void)ui_library_input(&lib, UI_BTN_BACK, 5);
    fake_reset();
    ui_library_draw(&fake_draw, &lib, 0, NULL);
    TEST_ASSERT_EQUAL_INT(1, count_kind('s'));
    TEST_ASSERT_TRUE(fake_find_text("LEAVE") >= 0);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_open);
    RUN_TEST(test_install_asks_first);
    RUN_TEST(test_back_asks_before_leaving);
    RUN_TEST(test_sections);
    RUN_TEST(test_empty);
    RUN_TEST(test_draw);
    RUN_TEST(test_stopped);
    RUN_TEST(test_unfinished);
    RUN_TEST(test_installed_section);
    RUN_TEST(test_dialog_over_backdrop);
    return UNITY_END();
}
