#ifndef XCAT_UI_LIBRARY_H
#define XCAT_UI_LIBRARY_H

/*
 * The library screen (M8, docs/ui-style.md#library): the sections on the left, the games in a
 * list, the highlighted game's details beside it, and a footer. A asks before installing the
 * highlighted game (the map is a catalog: nothing installs unasked), and Back asks before
 * leaving for the dashboard. Only GAMES works so far; the other sections say so. Plain C
 * over ui_draw like the widgets, tested on the PC (test_ui_library).
 */

#include <stdbool.h>

#include "input.h"
#include "ui.h"
#include "widgets.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ui_lib_state {
    UI_LIB_NOT_TRIED, /* not installed in this run */
    UI_LIB_INSTALLED, /* installed and verified in this run */
    UI_LIB_FAILED,    /* tried in this run, and it failed */
    UI_LIB_STOPPED,   /* tried in this run, and the user stopped it */
} ui_lib_state;

/* One game, as the caller describes it. The strings are the caller's and must stay alive. */
typedef struct ui_lib_item {
    const char *name;  /* the list's line and the details' heading: any UTF-8 */
    const char *facts; /* under the heading, small: year, region, size; may be "" */
    const char *about; /* wrapped below: who made it, the release, where it comes from */
    ui_lib_state state;
    const char *note; /* after an install in this run: what happened; may be NULL */
} ui_lib_item;

typedef enum ui_lib_section {
    UI_LIB_GAMES,
    UI_LIB_DOWNLOADS,
    UI_LIB_INSTALLED_GAMES,
    UI_LIB_SETTINGS,
    UI_LIB_SECTION_COUNT_
} ui_lib_section;

typedef enum ui_lib_action {
    UI_LIB_NONE,    /* redraw if the screen says it changed */
    UI_LIB_INSTALL, /* install items[list.cur]: the user said yes */
    UI_LIB_EXIT,    /* back to the dashboard: the user said yes */
} ui_lib_action;

typedef struct ui_library {
    const ui_lib_item *items;
    int count;
    ui_list list;
    ui_lib_section section;
    bool in_sections;     /* the D-pad moves between the sections, not the games */
    ui_lib_action asking; /* what the open dialog asks; UI_LIB_NONE: none is open */
    bool backdrop;        /* the screen behind the dialog is saved (ui_backdrop_save) */
    ui_dialog dialog;
    char message[256]; /* the dialog's */
} ui_library;

/* Starts the screen on the first game of GAMES. */
void ui_library_open(ui_library *lib, const ui_lib_item *items, int count);

/* How many list lines the screen shows. */
int ui_library_rows(const ui_draw *d);

/* One button press. In the list, the D-pad, LT and RT move (ui_list_input), Left goes to the
   sections, A asks to install the highlighted game and Back asks to leave. In the sections,
   Up and Down choose one, and Right or A go back to the list. In a dialog, its buttons. */
ui_lib_action ui_library_input(ui_library *lib, ui_button b, int rows);

/* Draws the screen with the background at pan (ui_pan_x), and `status` (may be NULL) in the
   footer. Over an open dialog, the screen behind stands still (ui_backdrop). */
void ui_library_draw(const ui_draw *d, ui_library *lib, int pan, const char *status);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_UI_LIBRARY_H */
