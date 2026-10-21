#ifndef XCAT_UI_DRAW_SDL_H
#define XCAT_UI_DRAW_SDL_H

/*
 * The ui_draw backend on the Xbox (ADR 0015 decision 1): one 640x480 SDL window and
 * renderer, text with SDL_ttf in the bundled Noto Sans, and rendered strings cached as
 * textures so a frame doesn't rasterise its text again. Only *_sdl.c files include SDL.
 */

#include <stdbool.h>

#include "ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opens the window, the renderer and the fonts in `font_dir` (NotoSans-Regular.ttf and
   NotoSans-Bold.ttf, third_party/fonts) and fills `d`. On failure returns false and sets
   *why to a message for the debug console. Call after XVideoSetMode. */
bool ui_sdl_open(ui_draw *d, const char *font_dir, const char **why);

/* Starts a frame: clears the screen to `bg`. */
void ui_sdl_begin(ui_color bg);

/* Shows the frame drawn since ui_sdl_begin. */
void ui_sdl_present(void);

/* Text cache statistics since ui_sdl_open, for the ui-test XBE: lookups and misses. */
void ui_sdl_cache_stats(unsigned long *lookups, unsigned long *misses);

void ui_sdl_close(void);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_UI_DRAW_SDL_H */
