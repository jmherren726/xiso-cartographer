#ifndef XCAT_UI_INPUT_H
#define XCAT_UI_INPUT_H

/*
 * The controller's buttons as the widgets see them (ADR 0015 decision 2), and the repeat of
 * a held D-pad direction. Portable: input_sdl.c turns SDL's events into ui_buttons on the
 * Xbox, the host tests make them up.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ui_button {
    UI_BTN_NONE,
    UI_BTN_UP,
    UI_BTN_DOWN,
    UI_BTN_LEFT,
    UI_BTN_RIGHT,
    UI_BTN_A, /* chooses */
    UI_BTN_B, /* goes back */
    UI_BTN_X,
    UI_BTN_Y, /* opens search */
    UI_BTN_START,
    UI_BTN_BACK, /* leaves the app */
    UI_BTN_LT,   /* the triggers, pressed past half way */
    UI_BTN_RT,
    UI_BTN_WHITE,
    UI_BTN_BLACK,
    UI_BTN_COUNT_
} ui_button;

/* A held D-pad direction presses again after UI_REPEAT_DELAY_MS, then every
   UI_REPEAT_EVERY_MS until it's let go. Other buttons never repeat: holding A must not
   choose twice. */
#define UI_REPEAT_DELAY_MS 400u
#define UI_REPEAT_EVERY_MS 80u

/* Which direction is held, and when it presses next. Zero it to start. */
typedef struct ui_repeat {
    ui_button held;
    uint32_t next_ms;
} ui_repeat;

/* True for the buttons that repeat: the D-pad's four directions. */
bool ui_button_repeats(ui_button b);

/* Button b went down at now_ms. A direction starts repeating; it replaces the one held. */
void ui_repeat_down(ui_repeat *r, ui_button b, uint32_t now_ms);

/* Button b went up: if it's the one held, it stops repeating. */
void ui_repeat_up(ui_repeat *r, ui_button b);

/* The held direction if it's due to press again at now_ms (then the next press is
   scheduled), else UI_BTN_NONE. Times may wrap around 2^32 ms. */
ui_button ui_repeat_due(ui_repeat *r, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_UI_INPUT_H */
