/*
 * UI test XBE (M8, ADR 0015, docs/ui-style.md): draws the theme, the bundled font and the
 * text helpers through the SDL backend, so they can be checked in xemu and on hardware
 * before the real screens exist.
 *
 * One screen laid out like the library: the panorama panning behind everything, a title tab,
 * a column of sections, a list of 40 lines that the D-pad scrolls, and a detail panel with
 * Latin, Greek and Cyrillic text, characters the font lacks (drawn as boxes), a line cut with
 * an ellipsis and a wrapped paragraph. The bottom lines show the time to draw a frame, the
 * frames drawn per second and the text cache's hits. A frame is drawn only when something
 * changed: a button, the stats, or the background moving a pixel. Y switches to drawing
 * every frame. Back returns to the dashboard.
 */
#include <SDL.h>
#include <hal/debug.h>
#include <hal/video.h>
#include <stdbool.h>
#include <stdio.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include "draw_sdl.h"
#include "theme.h"
#include "ui.h"
#include "xcat/version.h"

#define LIST_LINES 40
#define PAD 10

static const char *const sections[] = {"GAMES", "DOWNLOADS", "INSTALLED", "SETTINGS"};

/* The rows of the list that fit in `list`, each `row_h` high. */
static int rows_shown(const ui_draw *d, ui_rect list)
{
    int row_h = d->line_height(d->ctx, UI_FONT_BODY) + 6;
    return (list.h - 2 * PAD) / row_h;
}

/* The title-safe area (32, 24, 576 x 432), split like the library screen. */
static const ui_rect tab = {32, 24, 240, 46};
static const ui_rect nav = {32, 80, 170, 196};
static const ui_rect list = {210, 80, 212, 320};
static const ui_rect detail = {430, 80, 178, 320};
static const ui_rect footer = {32, 408, 576, 48};

static void draw(const ui_draw *d, int pan, int cur, int top, const char *stats)
{
    ui_background(d, pan);

    ui_panel(d, tab, UI_CHAMFER, ui_col_panel, ui_col_border);
    int th = d->line_height(d->ctx, UI_FONT_TITLE);
    (void)ui_text_fit(d, UI_FONT_TITLE, tab.x + 22, tab.y + (tab.h - th) / 2, tab.w - 30,
                      ui_col_accent, "UI TEST");

    ui_panel(d, nav, UI_CHAMFER, ui_col_panel, ui_col_border);
    int bh = d->line_height(d->ctx, UI_FONT_BOLD);
    for (int i = 0; i < 4; i++) {
        ui_rect item = {nav.x + 8, nav.y + PAD + i * (bh + 16), nav.w - 16, bh + 10};
        if (i == 0) {
            ui_panel(d, item, 8, ui_col_highlight, ui_col_highlight_border);
        }
        (void)ui_text_fit(d, UI_FONT_BOLD, item.x + 8, item.y + 5, item.w - 12,
                          i == 0 ? ui_col_text : ui_col_dim, sections[i]);
    }

    ui_panel(d, list, UI_CHAMFER, ui_col_panel, ui_col_border);
    int lh = d->line_height(d->ctx, UI_FONT_BODY);
    int shown = rows_shown(d, list);
    d->clip(d->ctx, &list);
    for (int i = 0; i < shown && top + i < LIST_LINES; i++) {
        ui_rect row = {list.x + 8, list.y + PAD + i * (lh + 6), list.w - 16, lh + 4};
        char line[64];
        snprintf(line, sizeof line, "Test game %d", top + i + 1);
        if (top + i == cur) {
            ui_panel(d, row, 6, ui_col_highlight, ui_col_highlight_border);
        }
        (void)ui_text_fit(d, top + i == cur ? UI_FONT_BOLD : UI_FONT_BODY, row.x + 10, row.y + 2,
                          row.w - 18, ui_col_text, line);
    }
    d->clip(d->ctx, NULL);

    ui_panel(d, detail, UI_CHAMFER, ui_col_panel, ui_col_border);
    int x = detail.x + PAD + 4;
    int w = detail.w - 2 * PAD - 4;
    int y = detail.y + PAD;
    char name[32];
    snprintf(name, sizeof name, "Game %d", cur + 1);
    (void)ui_text_fit(d, UI_FONT_BOLD, x, y, w, ui_col_accent, name);
    y += bh;
    int sh = d->line_height(d->ctx, UI_FONT_SMALL);
    (void)ui_text_fit(d, UI_FONT_SMALL, x, y, w, ui_col_dim, "Test  |  2026  |  1.2 GB");
    y += sh + 6;
    /* Latin with accents; Greek and Cyrillic; CJK, which Noto Sans lacks. */
    (void)ui_text_fit(d, UI_FONT_SMALL, x, y, w, ui_col_text,
                      "Caf\xC3\xA9, na\xC3\xAFve, \xC3\x86r\xC3\xB8sk\xC3\xB8"
                      "bing");
    y += sh;
    (void)ui_text_fit(d, UI_FONT_SMALL, x, y, w, ui_col_text,
                      "\xCE\x9A\xCE\xB1\xCE\xBB\xCE\xB7\xCE\xBC\xCE\xAD\xCF\x81\xCE\xB1 "
                      "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82");
    y += sh;
    (void)ui_text_fit(d, UI_FONT_SMALL, x, y, w, ui_col_dim, "Boxes: \xE6\x97\xA5\xE6\x9C\xAC");
    y += sh + 6;
    (void)ui_text_fit(d, UI_FONT_BODY, x, y, w, ui_col_text,
                      "Cut here: a long game title that does not fit");
    y += lh + 6;
    ui_rect para = {x, y, w, detail.y + detail.h - PAD - y};
    (void)ui_text_wrap(d, UI_FONT_SMALL, para, ui_col_dim,
                       "Wrapped: the words break at spaces, a very-long-word-like-this is cut "
                       "between characters, and text that doesn't fit ends with an ellipsis, "
                       "like this paragraph, which goes on for longer than the panel has room "
                       "for, so its last line is cut.");

    ui_panel(d, footer, 0, ui_col_panel, ui_col_border);
    int by = footer.y + (footer.h - 2 * sh) / 2;
    (void)ui_text_fit(d, UI_FONT_SMALL, footer.x + 12, by, footer.w - 24, ui_col_dim, stats);
    char hints[96];
    snprintf(hints, sizeof hints, "%d / %d    D-pad: list    Y: draw every frame    Back: exit",
             cur + 1, LIST_LINES);
    (void)ui_text_fit(d, UI_FONT_SMALL, footer.x + 12, by + sh, footer.w - 24, ui_col_text, hints);
}

int main(void)
{
    XVideoSetMode(UI_SCREEN_W, UI_SCREEN_H, 32, REFRESH_DEFAULT);
    ui_draw d;
    const char *why = NULL;
    if (SDL_Init(SDL_INIT_GAMECONTROLLER) != 0 || !ui_sdl_open(&d, "D:", &why)) {
        debugPrint("UI test: %s\n", why != NULL ? why : SDL_GetError());
        for (;;) {
            Sleep(1000);
        }
    }
    int cur = 0;
    int top = 0;
    int shown = rows_shown(&d, list);
    int range = ui_pan_range(&d);
    int pan = -1;
    bool continuous = false;
    bool dirty = true;
    char stats[160] = "measuring...";
    Uint32 second_start = SDL_GetTicks();
    Uint32 draw_ms_sum = 0;
    int frames = 0;
    for (;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_CONTROLLERDEVICEADDED) {
                (void)SDL_GameControllerOpen(e.cdevice.which);
            } else if (e.type == SDL_CONTROLLERBUTTONDOWN) {
                dirty = true;
                if (e.cbutton.button == SDL_CONTROLLER_BUTTON_BACK) {
                    ui_sdl_close();
                    HalReturnToFirmware(HalQuickRebootRoutine);
                } else if (e.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_UP && cur > 0) {
                    cur--;
                } else if (e.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN &&
                           cur + 1 < LIST_LINES) {
                    cur++;
                } else if (e.cbutton.button == SDL_CONTROLLER_BUTTON_Y) {
                    continuous = !continuous;
                }
            }
        }
        if (cur < top) {
            top = cur;
        } else if (cur >= top + shown) {
            top = cur - shown + 1;
        }
        int now_pan = ui_pan_x(SDL_GetTicks(), range);
        if (now_pan != pan) {
            pan = now_pan;
            dirty = true;
        }
        if (dirty || continuous) {
            Uint32 start = SDL_GetTicks();
            ui_sdl_begin(ui_col_bg);
            draw(&d, pan, cur, top, stats);
            draw_ms_sum += SDL_GetTicks() - start;
            ui_sdl_present();
            frames++;
            dirty = false;
        } else {
            Sleep(5);
        }
        Uint32 now = SDL_GetTicks();
        if (now - second_start >= 1000U) {
            unsigned long lookups = 0;
            unsigned long misses = 0;
            ui_sdl_cache_stats(&lookups, &misses);
            snprintf(stats, sizeof stats,
                     "%s: %d frames/s, %lu ms to draw. Pan %d of %d. Cache %lu/%lu missed",
                     continuous ? "Every frame" : "On change", frames,
                     frames > 0 ? (unsigned long)(draw_ms_sum / (Uint32)frames) : 0UL, pan, range,
                     misses, lookups);
            frames = 0;
            draw_ms_sum = 0;
            second_start = now;
            dirty = true; /* the stats line changed */
        }
    }
}
