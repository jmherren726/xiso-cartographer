#ifndef XCAT_UI_INPUT_SDL_H
#define XCAT_UI_INPUT_SDL_H

/*
 * The controllers through nxdk's SDL (ADR 0015): every controller plugged in, its buttons
 * as ui_buttons, and held D-pad directions repeated (input.h). Only *_sdl.c files include
 * SDL, so this header doesn't.
 */

#include <stdbool.h>

#include "input.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The next button pressed on any controller, a held direction's repeat included, or
   UI_BTN_NONE when nothing is waiting. Opens controllers as they're plugged in. Call after
   SDL_Init(SDL_INIT_GAMECONTROLLER), and often: it reads SDL's events. */
ui_button ui_sdl_button(void);

/* Whether LT + RT + Back + Black (the dashboard shortcut of other homebrew) are held together
   on any controller opened by ui_sdl_button. */
bool ui_sdl_exit_combo(void);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_UI_INPUT_SDL_H */
