/*
 * UI test XBE (M8, ADR 0015): draws the bundled font and the text helpers through the SDL
 * backend, so they can be checked in xemu and on hardware before the real screens exist.
 *
 * One screen: the title-safe area outlined, the four fonts, Latin, Greek and Cyrillic text,
 * characters the font lacks (drawn as boxes), a cut line with an ellipsis, a wrapped
 * paragraph, and a list of 40 lines that the D-pad scrolls. The bottom line shows the time to
 * draw a frame and the text cache's hits; Y switches between drawing on change and drawing
 * every frame (with frames per second). Back returns to the dashboard.
 */
#include <SDL.h>
#include <hal/debug.h>
#include <hal/video.h>
#include <stdbool.h>
#include <stdio.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include "draw_sdl.h"
#include "ui.h"
#include "xcat/version.h"

#define LIST_LINES 40
#define LIST_SHOWN 6

static const ui_color bg = {16, 24, 32, 255};
static const ui_color fg = {230, 230, 230, 255};
static const ui_color dim = {150, 160, 170, 255};
static const ui_color accent = {110, 200, 90, 255};
static const ui_color highlight = {50, 80, 110, 255};
static const ui_color edge = {90, 90, 90, 255};

static void outline(const ui_draw *d, ui_rect r, ui_color c)
{
    d->fill(d->ctx, (ui_rect){r.x, r.y, r.w, 1}, c);
    d->fill(d->ctx, (ui_rect){r.x, r.y + r.h - 1, r.w, 1}, c);
    d->fill(d->ctx, (ui_rect){r.x, r.y, 1, r.h}, c);
    d->fill(d->ctx, (ui_rect){r.x + r.w - 1, r.y, 1, r.h}, c);
}

static void draw(const ui_draw *d, int cur, int top, const char *stats)
{
    ui_rect safe = ui_safe_area();
    outline(d, safe, edge);
    int x = safe.x + 8;
    int y = safe.y + 4;
    int w = safe.w - 16;
    char title[96];
    snprintf(title, sizeof title, "xiso-cartographer %s: UI test", xc_version_string());
    (void)ui_text_fit(d, UI_FONT_TITLE, x, y, w, accent, title);
    y += d->line_height(d->ctx, UI_FONT_TITLE) + 4;

    /* Latin with accents; Greek and Cyrillic; CJK, which Noto Sans lacks. */
    (void)ui_text_fit(d, UI_FONT_BODY, x, y, w, fg,
                      "Body: Caf\xC3\xA9, na\xC3\xAFve, \xC3\x86r\xC3\xB8sk\xC3\xB8"
                      "bing");
    y += d->line_height(d->ctx, UI_FONT_BODY);
    (void)ui_text_fit(d, UI_FONT_BOLD, x, y, w, fg,
                      "Bold: \xCE\x9A\xCE\xB1\xCE\xBB\xCE\xB7\xCE\xBC\xCE\xAD\xCF\x81\xCE\xB1 "
                      "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82");
    y += d->line_height(d->ctx, UI_FONT_BOLD);
    (void)ui_text_fit(d, UI_FONT_SMALL, x, y, w, dim,
                      "Small: not in the font, drawn as boxes: \xE6\x97\xA5\xE6\x9C\xAC");
    y += d->line_height(d->ctx, UI_FONT_SMALL) + 4;

    (void)ui_text_fit(d, UI_FONT_BODY, x, y, 300, fg,
                      "Cut at 300 pixels: a long game title that does not fit on one line");
    outline(d, (ui_rect){x, y, 300, d->line_height(d->ctx, UI_FONT_BODY)}, edge);
    y += d->line_height(d->ctx, UI_FONT_BODY) + 4;

    ui_rect para = {x, y, 260, 3 * d->line_height(d->ctx, UI_FONT_SMALL)};
    outline(d, para, edge);
    (void)ui_text_wrap(d, UI_FONT_SMALL, para, dim,
                       "Wrapped into three lines of 260 pixels: the words break at spaces, a "
                       "very-long-word-like-this is cut between characters, and text that "
                       "doesn't fit ends with an ellipsis.");

    ui_rect list = {x + 280, y, safe.w - 296, LIST_SHOWN * d->line_height(d->ctx, UI_FONT_BODY)};
    outline(d, list, edge);
    d->clip(d->ctx, &list);
    for (int i = 0; i < LIST_SHOWN && top + i < LIST_LINES; i++) {
        int ly = list.y + i * d->line_height(d->ctx, UI_FONT_BODY);
        char line[64];
        snprintf(line, sizeof line, "Game number %d, a list line", top + i + 1);
        if (top + i == cur) {
            d->fill(d->ctx, (ui_rect){list.x, ly, list.w, d->line_height(d->ctx, UI_FONT_BODY)},
                    highlight);
        }
        (void)ui_text_fit(d, top + i == cur ? UI_FONT_BOLD : UI_FONT_BODY, list.x + 6, ly,
                          list.w - 12, fg, line);
    }
    d->clip(d->ctx, NULL);

    int by = safe.y + safe.h - d->line_height(d->ctx, UI_FONT_SMALL) - 4;
    (void)ui_text_fit(d, UI_FONT_SMALL, x, by, w, dim, stats);
}

int main(void)
{
    XVideoSetMode(UI_SCREEN_W, UI_SCREEN_H, 32, REFRESH_DEFAULT);
    ui_draw d;
    const char *why = NULL;
    if (SDL_Init(SDL_INIT_GAMECONTROLLER) != 0 || !ui_sdl_open(&d, "D:\\fonts", &why)) {
        debugPrint("UI test: %s\n", why != NULL ? why : SDL_GetError());
        for (;;) {
            Sleep(1000);
        }
    }
    int cur = 0;
    int top = 0;
    /* A frame is drawn only when something changed, as the screens will. Y switches to
       drawing every frame, to measure the frame rate. */
    bool continuous = false;
    bool dirty = true;
    char stats[160] = "measuring...";
    Uint32 second_start = SDL_GetTicks();
    Uint32 draw_ms_sum = 0;
    Uint32 last_draw_ms = 0;
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
        } else if (cur >= top + LIST_SHOWN) {
            top = cur - LIST_SHOWN + 1;
        }
        if (dirty || continuous) {
            Uint32 start = SDL_GetTicks();
            ui_sdl_begin(bg);
            draw(&d, cur, top, stats);
            last_draw_ms = SDL_GetTicks() - start;
            draw_ms_sum += last_draw_ms;
            ui_sdl_present();
            frames++;
            dirty = false;
        } else {
            Sleep(10);
        }
        Uint32 now = SDL_GetTicks();
        if (now - second_start >= 1000U) {
            unsigned long lookups = 0;
            unsigned long misses = 0;
            ui_sdl_cache_stats(&lookups, &misses);
            if (continuous) {
                snprintf(stats, sizeof stats,
                         "Every frame: %d frames/s, %lu ms to draw. Cache %lu lookups %lu "
                         "misses. Y: on change",
                         frames, (unsigned long)(draw_ms_sum / (Uint32)frames), lookups, misses);
            } else {
                snprintf(stats, sizeof stats,
                         "On change: last frame %lu ms to draw. Cache %lu lookups %lu misses. "
                         "D-pad: list, Y: every frame, Back: dashboard",
                         (unsigned long)last_draw_ms, lookups, misses);
            }
            frames = 0;
            draw_ms_sum = 0;
            second_start = now;
            dirty = true; /* the stats line changed */
        }
    }
}
