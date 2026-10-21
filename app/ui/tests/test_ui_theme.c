/* Tests for the theme (app/ui/theme.h, theme.c) on a fake backend that paints a pixel grid. */
#include "theme.h"
#include "unity.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* ---- A fake backend: fills paint characters into a grid; images are logged. */

#define GW 64
#define GH 48

static char grid[GH][GW];
static int fill_count[GH][GW]; /* how often the panel's fill colour painted each pixel */
static int image_w, image_h;   /* the fake background's size; 0 x 0: not loaded */
static ui_rect image_src;
static int image_calls;

static const ui_color fill_c = {1, 2, 3, 100};
static const ui_color border_c = {9, 9, 9, 255};

static void fake_fill(void *ctx, ui_rect r, ui_color c)
{
    (void)ctx;
    char mark = 'o'; /* another colour */
    if (memcmp(&c, &fill_c, sizeof c) == 0) {
        mark = 'f';
    } else if (memcmp(&c, &border_c, sizeof c) == 0) {
        mark = 'B';
    }
    for (int y = r.y; y < r.y + r.h; y++) {
        for (int x = r.x; x < r.x + r.w; x++) {
            if (x >= 0 && y >= 0 && x < GW && y < GH) {
                grid[y][x] = mark;
                if (mark == 'f') {
                    fill_count[y][x]++;
                }
            }
        }
    }
}

static void fake_image(void *ctx, ui_image img, ui_rect src, int x, int y)
{
    (void)ctx;
    (void)img;
    (void)x;
    (void)y;
    image_src = src;
    image_calls++;
}

static void fake_image_size(void *ctx, ui_image img, int *w, int *h)
{
    (void)ctx;
    (void)img;
    *w = image_w;
    *h = image_h;
}

static const ui_draw fake = {NULL, fake_fill, NULL, NULL, NULL, NULL, fake_image, fake_image_size};

static void clear(void)
{
    memset(grid, '.', sizeof grid);
    memset(fill_count, 0, sizeof fill_count);
}

/* ---- Tests */

static void test_pan_ends_and_middle(void)
{
    TEST_ASSERT_EQUAL_INT(0, ui_pan_x(0, 800));
    TEST_ASSERT_EQUAL_INT(400, ui_pan_x(UI_PAN_LEG_MS / 2, 800));
    TEST_ASSERT_EQUAL_INT(800, ui_pan_x(UI_PAN_LEG_MS, 800));
    TEST_ASSERT_EQUAL_INT(400, ui_pan_x(UI_PAN_LEG_MS + UI_PAN_LEG_MS / 2, 800));
    TEST_ASSERT_EQUAL_INT(0, ui_pan_x(2 * UI_PAN_LEG_MS, 800));
    TEST_ASSERT_EQUAL_INT(0, ui_pan_x(12345, 0));
    TEST_ASSERT_EQUAL_INT(0, ui_pan_x(12345, -5));
}

static void test_pan_is_smooth_and_symmetric(void)
{
    int prev = 0;
    int max_step = 0;
    for (uint32_t t = 0; t <= UI_PAN_LEG_MS; t += 50) {
        int x = ui_pan_x(t, 800);
        TEST_ASSERT_TRUE(x >= prev); /* never backwards within a leg */
        if (x - prev > max_step) {
            max_step = x - prev;
        }
        prev = x;
        if (t < UI_PAN_LEG_MS) { /* the way back mirrors the way there */
            TEST_ASSERT_EQUAL_INT(800 - x, ui_pan_x(UI_PAN_LEG_MS + t, 800));
        }
    }
    /* Smoothstep peaks at 1.5x the mean speed: 800 px in 60 s is 13 px/s, so 20 px/s at the
       most, 1 px per 50 ms. */
    TEST_ASSERT_TRUE(max_step <= 2);
    /* Slow at the ends: the first second moves less than 1 % of the way. */
    TEST_ASSERT_TRUE(ui_pan_x(1000, 800) < 8);
}

static void test_background(void)
{
    clear();
    image_w = 0;
    image_h = 0;
    image_calls = 0;
    TEST_ASSERT_EQUAL_INT(0, ui_pan_range(&fake));
    ui_background(&fake, 100); /* no image: a fill */
    TEST_ASSERT_EQUAL_INT(0, image_calls);
    TEST_ASSERT_EQUAL_CHAR('o', grid[0][0]);

    image_w = 1440;
    image_h = 480;
    TEST_ASSERT_EQUAL_INT(800, ui_pan_range(&fake));
    ui_background(&fake, 100);
    TEST_ASSERT_EQUAL_INT(1, image_calls);
    TEST_ASSERT_EQUAL_INT(100, image_src.x);
    TEST_ASSERT_EQUAL_INT(UI_SCREEN_W, image_src.w);
    TEST_ASSERT_EQUAL_INT(UI_SCREEN_H, image_src.h);
    ui_background(&fake, 5000); /* clamped */
    TEST_ASSERT_EQUAL_INT(800, image_src.x);
    ui_background(&fake, -3);
    TEST_ASSERT_EQUAL_INT(0, image_src.x);

    image_w = 600; /* narrower than the screen: not used */
    TEST_ASSERT_EQUAL_INT(0, ui_pan_range(&fake));
}

static void test_panel_shape(void)
{
    clear();
    ui_panel(&fake, (ui_rect){2, 3, 30, 20}, 6, fill_c, border_c);
    /* Cut corners: top left and bottom right stay empty, the others are square. */
    TEST_ASSERT_EQUAL_CHAR('.', grid[3][2]);
    TEST_ASSERT_EQUAL_CHAR('.', grid[3 + 19][2 + 29]);
    TEST_ASSERT_EQUAL_CHAR('B', grid[3][2 + 29]);      /* top right */
    TEST_ASSERT_EQUAL_CHAR('B', grid[3 + 19][2]);      /* bottom left */
    TEST_ASSERT_EQUAL_CHAR('B', grid[3][2 + 6]);       /* where the top edge starts */
    TEST_ASSERT_EQUAL_CHAR('.', grid[3][2 + 5]);       /* left of it, cut */
    TEST_ASSERT_EQUAL_CHAR('B', grid[3 + 6][2]);       /* where the left edge starts */
    TEST_ASSERT_EQUAL_CHAR('f', grid[3 + 10][2 + 15]); /* inside */
    /* The diagonals are closed: every row has border at both ends, and nothing outside. */
    for (int y = 3; y < 3 + 20; y++) {
        int first = -1;
        int last = -1;
        for (int x = 0; x < GW; x++) {
            if (grid[y][x] != '.') {
                if (first < 0) {
                    first = x;
                }
                last = x;
            }
        }
        TEST_ASSERT_EQUAL_CHAR('B', grid[y][first]);
        TEST_ASSERT_EQUAL_CHAR('B', grid[y][last]);
        for (int x = first; x <= last; x++) {
            TEST_ASSERT_TRUE(grid[y][x] != '.');     /* no holes */
            TEST_ASSERT_TRUE(fill_count[y][x] <= 1); /* translucent fill painted once */
        }
    }
    TEST_ASSERT_EQUAL_CHAR('.', grid[2][10]);
    TEST_ASSERT_EQUAL_CHAR('.', grid[23][10]);
}

static void test_panel_square_and_tiny(void)
{
    clear();
    ui_panel(&fake, (ui_rect){0, 0, 10, 8}, 0, fill_c, border_c);
    TEST_ASSERT_EQUAL_CHAR('B', grid[0][0]);
    TEST_ASSERT_EQUAL_CHAR('B', grid[7][9]);
    TEST_ASSERT_EQUAL_CHAR('f', grid[4][4]);
    clear();
    ui_panel(&fake, (ui_rect){0, 0, 6, 4}, 50, fill_c, border_c); /* chamfer clamped */
    for (int y = 0; y < GH; y++) {
        for (int x = 0; x < GW; x++) {
            TEST_ASSERT_TRUE(fill_count[y][x] <= 1);
            if (x >= 6 || y >= 4) {
                TEST_ASSERT_EQUAL_CHAR('.', grid[y][x]);
            }
        }
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_pan_ends_and_middle);
    RUN_TEST(test_pan_is_smooth_and_symmetric);
    RUN_TEST(test_background);
    RUN_TEST(test_panel_shape);
    RUN_TEST(test_panel_square_and_tiny);
    return UNITY_END();
}
