#ifndef XCAT_UI_DRAW_SDL_H
#define XCAT_UI_DRAW_SDL_H

/*
 * The ui_draw backend on the Xbox (ADR 0015 decision 1): one 640x480 SDL window and
 * renderer, text with SDL_ttf in the bundled fonts, and rendered strings cached as textures
 * so a frame doesn't rasterise its text again. Frames are drawn straight into two cached
 * framebuffers, one shown while the other is drawn. Translucent fills, images and the backdrop
 * are done by the backend's own pixel loops rather than SDL's (docs/ui-style.md#cost). Only
 * *_sdl.c files include SDL.
 */

#include <stdbool.h>
#include <stdint.h>

#include "ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opens the window and the renderer, loads the assets from `asset_dir` (the XBE's folder,
   app/ui/ui-assets.mk) and fills `d`: the fonts from fonts\ (NotoSans-Regular.ttf and
   NotoSans-Bold.ttf, third_party/fonts), which must be there, and the images from ui\
   (ui_image), which may be missing. On failure returns false and sets *why to a message
   for the debug console. Call after XVideoSetMode. */
bool ui_sdl_open(ui_draw *d, const char *asset_dir, const char **why);

/* Starts a frame. Without the background image it clears the screen to `bg`; with it, the
   frame must start with ui_background or ui_backdrop, which cover the whole screen, and the
   clear would only cost another pass over it. */
void ui_sdl_begin(ui_color bg);

/* Shows the frame drawn since ui_sdl_begin, at the next vertical blank (so it waits up to a
   refresh). Screens draw a frame only when something on them changed: every frame is drawn
   whole by the CPU, which an install needs for itself. */
void ui_sdl_present(void);

/* Where a frame's time went, in microseconds, for the ui-test XBE. */
typedef struct ui_sdl_times {
    uint32_t copy_us;   /* while drawing: copying images and the backdrop */
    uint32_t blend_us;  /* while drawing: translucent fills */
    uint32_t finish_us; /* in ui_sdl_present: SDL's commands still queued (text, opaque fills) */
    uint32_t flip_us;   /* in ui_sdl_present: getting the frame to memory for the screen */
} ui_sdl_times;

/* The last presented frame's times; and a microsecond clock. */
void ui_sdl_last_times(ui_sdl_times *t);
uint32_t ui_sdl_us(void);

/* Text cache statistics since ui_sdl_open, for the ui-test XBE: lookups and misses. */
void ui_sdl_cache_stats(unsigned long *lookups, unsigned long *misses);

void ui_sdl_close(void);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_UI_DRAW_SDL_H */
