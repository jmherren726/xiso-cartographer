/* The repeat of a held D-pad direction (input.h). Portable: no SDL or nxdk header. */
#include "input.h"

bool ui_button_repeats(ui_button b)
{
    return b == UI_BTN_UP || b == UI_BTN_DOWN || b == UI_BTN_LEFT || b == UI_BTN_RIGHT;
}

void ui_repeat_down(ui_repeat *r, ui_button b, uint32_t now_ms)
{
    if (ui_button_repeats(b)) {
        r->held = b;
        r->next_ms = now_ms + UI_REPEAT_DELAY_MS;
    }
}

void ui_repeat_up(ui_repeat *r, ui_button b)
{
    if (r->held == b) {
        r->held = UI_BTN_NONE;
    }
}

ui_button ui_repeat_due(ui_repeat *r, uint32_t now_ms)
{
    /* now >= next, counted so that it holds across the wrap at 2^32 ms (49.7 days). */
    if (r->held == UI_BTN_NONE || now_ms - r->next_ms >= 0x80000000u) {
        return UI_BTN_NONE;
    }
    r->next_ms = now_ms + UI_REPEAT_EVERY_MS;
    return r->held;
}
