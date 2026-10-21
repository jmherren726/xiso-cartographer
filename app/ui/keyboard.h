#ifndef XCAT_UI_KEYBOARD_H
#define XCAT_UI_KEYBOARD_H

/*
 * The on-screen keyboard (ADR 0015 decision 3): a grid of 10 x 5 keys with three layers,
 * small letters, capitals and symbols, which between them type every printable ASCII
 * character. The bottom row holds SHIFT, SYMBOLS, SPACE, DELETE and DONE.
 *
 * The D-pad moves (wrapping around the edges), A types the key, X deletes, Y adds a space,
 * LT switches capitals on and off, RT the symbols, Start finishes and B cancels.
 *
 * It edits the caller's buffer in place, adding at the end, and keeps no copy. A password
 * field shows a dot per character and never passes the text to the drawing backend, so it
 * reaches no texture or cache; the caller wipes the buffer when done (ADR 0010 decision 2).
 * Nothing here logs.
 */

#include <stdbool.h>
#include <stddef.h>

#include "input.h"
#include "ui.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_KBD_COLS 10
#define UI_KBD_ROWS 5

typedef enum ui_kbd_layer { UI_KBD_LOWER, UI_KBD_UPPER, UI_KBD_SYMBOLS } ui_kbd_layer;

typedef enum ui_kbd_result {
    UI_KBD_EDITING, /* still open */
    UI_KBD_DONE,    /* Start, or the DONE key: buf holds the text */
    UI_KBD_CANCEL   /* B: buf holds what was typed so far, for the caller to drop */
} ui_kbd_result;

typedef struct ui_kbd {
    const char *label; /* what the field is for: the app's own words, in capitals */
    char *buf;         /* the caller's, NUL-terminated */
    size_t cap;        /* buf's size: at most cap - 1 bytes of text */
    bool password;     /* show dots */
    ui_kbd_layer layer;
    int row, col; /* the highlighted key */
} ui_kbd;

/* Opens the keyboard on buf, keeping the text already in it, with the small letters shown
   and the highlight on "q". cap must be at least 1. */
void ui_kbd_open(ui_kbd *k, const char *label, char *buf, size_t cap, bool password);

/* Handles a press. */
ui_kbd_result ui_kbd_input(ui_kbd *k, ui_button b);

/* Dims the screen, then draws the keyboard in a panel centred on it: the label, the field
   (its end, if the text is wider), the keys and a line of hints. */
void ui_kbd_draw(const ui_draw *d, const ui_kbd *k);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_UI_KEYBOARD_H */
