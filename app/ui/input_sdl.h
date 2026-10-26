#ifndef XCAT_UI_INPUT_SDL_H
#define XCAT_UI_INPUT_SDL_H

/*
 * The controllers through nxdk's SDL (ADR 0015): every controller plugged in, its buttons
 * as ui_buttons, and held D-pad directions repeated (input.h). Only *_sdl.c files include
 * SDL, so this header doesn't.
 */

#include "input.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The next button pressed on any controller, a held direction's repeat included, or
   UI_BTN_NONE when nothing is waiting. Opens controllers as they're plugged in. Call after
   SDL_Init(SDL_INIT_GAMECONTROLLER), and often: it reads SDL's events. */
ui_button ui_sdl_button(void);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_UI_INPUT_SDL_H */
