/*
 * UI test XBE (M8, ADR 0015, docs/ui-style.md): draws the theme, the bundled fonts, the text
 * helpers and the widgets through the SDL backend, so they can be checked in xemu and on
 * hardware before the real screens exist.
 *
 * One screen laid out like the library: the panorama panning behind everything, a title tab,
 * a column of sections, a progress bar that fills over 20 seconds, a list of 40 lines that
 * the D-pad scrolls and LT / RT page, and a detail panel with Latin, Greek and Cyrillic text,
 * characters the font lacks (drawn as boxes), a line cut with an ellipsis and a wrapped
 * paragraph. A opens a dialog on the highlighted line, Y the keyboard for a search and X for
 * a password, over the screen darkened and standing still (ui_backdrop); the footer shows
 * what came back. The footer's first line shows the frames drawn per second and where a
 * frame's time goes (ui_sdl_times): drawing + SDL's work left at present + the flip, then the
 * parts of the drawing. A frame is drawn only when something changed: a button, the stats, the
 * progress or the background moving a pixel. White switches to drawing every frame. Back returns to
 * the dashboard.
 */
#include <SDL.h>
#include <hal/debug.h>
#include <hal/video.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include "draw_sdl.h"
#include "input_sdl.h"
#include "keyboard.h"
#include "theme.h"
#include "ui.h"
#include "widgets.h"
#include "xcat/version.h"

#define LIST_LINES 40
#define PAD 10
#define PROGRESS_MS 20000u /* the progress bar fills in this long, then starts again */
#define RESULT_MS 5000u    /* how long the footer shows what a dialog or keyboard returned */

static const char *const sections[] = {"GAMES", "DOWNLOADS", "INSTALLED", "SETTINGS"};
static const char *const choices[] = {"INSTALL", "DETAILS", "CANCEL"};

/* The title-safe area (32, 24, 576 x 432), split like the library screen. */
static const ui_rect tab = {32, 24, 240, 46};
static const ui_rect nav = {32, 80, 170, 196};
static const ui_rect progress = {32, 284, 170, 116};
static const ui_rect list = {210, 80, 212, 320};
static const ui_rect detail = {430, 80, 178, 320};
static const ui_rect footer = {32, 408, 576, 48};

/* The list's lines, inside its panel: 8 pixels in at the sides, PAD at the top and bottom. */
static const ui_rect list_in = {218, 90, 196, 300};

typedef enum mode { MODE_LIST, MODE_DIALOG, MODE_KEYBOARD } mode;

static const char *line_text(void *ctx, int i)
{
    static char line[32];
    (void)ctx;
    snprintf(line, sizeof line, "Test game %d", i + 1);
    return line;
}

static void draw_detail(const ui_draw *d, int cur)
{
    ui_panel(d, detail, UI_CHAMFER, ui_col_panel, ui_col_border);
    int x = detail.x + PAD + 4;
    int w = detail.w - 2 * PAD - 4;
    int y = detail.y + PAD;
    char name[32];
    snprintf(name, sizeof name, "Game %d", cur + 1);
    (void)ui_text_fit(d, UI_FONT_BOLD, x, y, w, ui_col_accent, name);
    y += d->line_height(d->ctx, UI_FONT_BOLD);
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
    int lh = d->line_height(d->ctx, UI_FONT_BODY);
    (void)ui_text_fit(d, UI_FONT_BODY, x, y, w, ui_col_text,
                      "Cut here: a long game title that does not fit");
    y += lh + 6;
    ui_rect para = {x, y, w, detail.y + detail.h - PAD - y};
    (void)ui_text_wrap(d, UI_FONT_SMALL, para, ui_col_dim,
                       "Wrapped: the words break at spaces, a very-long-word-like-this is cut "
                       "between characters, and text that doesn't fit ends with an ellipsis, "
                       "like this paragraph, which goes on for longer than the panel has room "
                       "for, so its last line is cut.");
}

static void draw(const ui_draw *d, int pan, const ui_list *l, uint32_t done, const char *stats,
                 const char *status)
{
    /* The panels cover their part of the background: it isn't copied twice. */
    const ui_rect panels[] = {tab, nav, progress, list, detail, footer};
    ui_background_under(d, pan, panels, (int)(sizeof panels / sizeof panels[0]));

    ui_panel(d, tab, UI_CHAMFER, ui_col_panel, ui_col_border);
    int th = d->line_height(d->ctx, UI_FONT_TITLE);
    (void)ui_text_fit(d, UI_FONT_TITLE, tab.x + 22, tab.y + (tab.h - th) / 2, tab.w - 30,
                      ui_col_accent, "UI TEST");

    ui_panel(d, nav, UI_CHAMFER, ui_col_panel, ui_col_border);
    int lbh = d->line_height(d->ctx, UI_FONT_LABEL);
    for (int i = 0; i < 4; i++) {
        ui_rect item = {nav.x + 8, nav.y + PAD + i * (lbh + 16), nav.w - 16, lbh + 10};
        if (i == 0) {
            ui_panel(d, item, 8, ui_col_highlight, ui_col_highlight_border);
        }
        (void)ui_text_fit(d, UI_FONT_LABEL, item.x + 10, item.y + 5, item.w - 14,
                          i == 0 ? ui_col_text : ui_col_dim, sections[i]);
    }

    ui_panel(d, progress, UI_CHAMFER, ui_col_panel, ui_col_border);
    (void)ui_text_fit(d, UI_FONT_LABEL, progress.x + 14, progress.y + PAD, progress.w - 24,
                      ui_col_accent, "DOWNLOAD");
    char pct[16];
    snprintf(pct, sizeof pct, "%d %%", ui_percent(done, PROGRESS_MS));
    ui_progress(d, (ui_rect){progress.x + 12, progress.y + PAD + lbh + 10, progress.w - 24, 34},
                done, PROGRESS_MS, pct);

    ui_panel(d, list, UI_CHAMFER, ui_col_panel, ui_col_border);
    ui_list_draw(d, l, list_in, line_text, NULL);

    draw_detail(d, l->cur);

    ui_panel(d, footer, 0, ui_col_panel, ui_col_border);
    int sh = d->line_height(d->ctx, UI_FONT_SMALL);
    int by = footer.y + (footer.h - 2 * sh) / 2;
    (void)ui_text_fit(d, UI_FONT_SMALL, footer.x + 12, by, footer.w - 24, ui_col_dim, stats);
    (void)ui_text_fit(d, UI_FONT_SMALL, footer.x + 12, by + sh, footer.w - 24, ui_col_text, status);
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
    int rows = ui_list_rows(&d, list_in);
    ui_list l = {LIST_LINES, 0, 0};
    mode m = MODE_LIST;
    ui_dialog dialog = {0};
    ui_kbd kbd;
    char search[64] = "";
    char password[64] = "";
    char result[128] = "";
    Uint32 result_at = 0;
    int range = ui_pan_range(&d);
    int pan = -1;
    int pct = -1;
    bool continuous = false;
    bool dirty = true;
    char stats[160] = "measuring...";
    Uint32 second_start = SDL_GetTicks();
    bool backdrop_saved = false; /* for the dialog or keyboard open */
    uint32_t draw_us_sum = 0;
    ui_sdl_times sum = {0};
    int frames = 0;
    for (;;) {
        ui_button b;
        while ((b = ui_sdl_button()) != UI_BTN_NONE) {
            dirty = true;
            if (b == UI_BTN_BACK) {
                ui_sdl_close();
                HalReturnToFirmware(HalQuickRebootRoutine);
            }
            if (b == UI_BTN_WHITE) {
                continuous = !continuous;
            } else if (m == MODE_DIALOG) {
                int c = ui_dialog_input(&dialog, b);
                if (c != UI_DIALOG_OPEN) {
                    snprintf(result, sizeof result, "Dialog: %s for Test game %d", choices[c],
                             l.cur + 1);
                    result_at = SDL_GetTicks();
                    m = MODE_LIST;
                }
            } else if (m == MODE_KEYBOARD) {
                ui_kbd_result r = ui_kbd_input(&kbd, b);
                if (r != UI_KBD_EDITING) {
                    if (kbd.password) {
                        snprintf(result, sizeof result, "Password: %s, %u characters (wiped)",
                                 r == UI_KBD_DONE ? "done" : "cancelled",
                                 (unsigned)strlen(password));
                        memset(password, 0, sizeof password);
                    } else {
                        snprintf(result, sizeof result, "Search: %s \"%s\"",
                                 r == UI_KBD_DONE ? "done" : "cancelled", search);
                    }
                    result_at = SDL_GetTicks();
                    m = MODE_LIST;
                }
            } else if (b == UI_BTN_A) {
                dialog = (ui_dialog){"INSTALL",
                                     "Install this test game? Nothing is installed: the choice "
                                     "is shown in the footer.",
                                     {choices[0], choices[1], choices[2]},
                                     0,
                                     2};
                m = MODE_DIALOG;
            } else if (b == UI_BTN_Y) {
                ui_kbd_open(&kbd, "SEARCH", search, sizeof search, false);
                m = MODE_KEYBOARD;
            } else if (b == UI_BTN_X) {
                ui_kbd_open(&kbd, "PASSWORD", password, sizeof password, true);
                m = MODE_KEYBOARD;
            } else {
                (void)ui_list_input(&l, b, rows);
            }
        }
        Uint32 now = SDL_GetTicks();
        int now_pan = ui_pan_x(now, range);
        uint32_t done = now % PROGRESS_MS;
        int now_pct = ui_percent(done, PROGRESS_MS);
        if (now_pan != pan || now_pct != pct) {
            pan = now_pan;
            pct = now_pct;
            dirty = true;
        }
        if (result[0] != '\0' && now - result_at >= RESULT_MS) {
            result[0] = '\0';
            dirty = true;
        }
        if (dirty || continuous) {
            char status[160];
            if (result[0] != '\0') {
                snprintf(status, sizeof status, "%s", result);
            } else {
                snprintf(status, sizeof status, "%s",
                         "A dialog  Y search  X password  LT RT page  White redraw  Back exit");
            }
            uint32_t start = ui_sdl_us();
            ui_sdl_begin(ui_col_bg);
            if (m == MODE_LIST || !backdrop_saved) {
                /* The bar shows the percentage it's labelled with, so they change together. */
                draw(&d, pan, &l, (uint32_t)pct * (PROGRESS_MS / 100), stats, status);
            }
            if (m == MODE_LIST) {
                backdrop_saved = false;
            } else if (!backdrop_saved) {
                ui_backdrop_save(&d);
                backdrop_saved = true;
            } else {
                ui_backdrop(&d);
            }
            if (m == MODE_DIALOG) {
                ui_dialog_draw(&d, &dialog);
            } else if (m == MODE_KEYBOARD) {
                ui_kbd_draw(&d, &kbd);
            }
            draw_us_sum += ui_sdl_us() - start;
            ui_sdl_present();
            ui_sdl_times t;
            ui_sdl_last_times(&t);
            sum.copy_us += t.copy_us;
            sum.blend_us += t.blend_us;
            sum.sdl_us += t.sdl_us;
            sum.size_us += t.size_us;
            sum.text_us += t.text_us;
            sum.finish_us += t.finish_us;
            sum.flip_us += t.flip_us;
            frames++;
            dirty = false;
        } else {
            Sleep(5);
        }
        now = SDL_GetTicks();
        if (now - second_start >= 1000U) {
            /* Means in tenths of a millisecond. */
            unsigned n = frames > 0 ? (unsigned)frames * 100U : 1U;
            unsigned v[] = {draw_us_sum / n,  sum.finish_us / n, sum.flip_us / n, sum.copy_us / n,
                            sum.blend_us / n, sum.sdl_us / n,    sum.size_us / n, sum.text_us / n};
            snprintf(stats, sizeof stats,
                     "%s %d/s %u.%u+%u.%u+%u.%u ms: copy %u.%u blend %u.%u sdl %u.%u size %u.%u "
                     "text %u.%u",
                     continuous ? "Every" : "Change", frames, v[0] / 10, v[0] % 10, v[1] / 10,
                     v[1] % 10, v[2] / 10, v[2] % 10, v[3] / 10, v[3] % 10, v[4] / 10, v[4] % 10,
                     v[5] / 10, v[5] % 10, v[6] / 10, v[6] % 10, v[7] / 10, v[7] % 10);
            frames = 0;
            draw_us_sum = 0;
            sum = (ui_sdl_times){0};
            second_start = now;
            dirty = true; /* the stats line changed */
        }
    }
}
