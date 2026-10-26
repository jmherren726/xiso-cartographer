#ifndef XCAT_UI_LIBRARY_H
#define XCAT_UI_LIBRARY_H

/*
 * The library screen (M8, docs/ui-style.md#library): the sections on the left, the games in a
 * list, the highlighted game's details beside it, and a footer. A asks before installing the
 * highlighted game (the map is a catalog: nothing installs unasked), and Back asks before
 * leaving for the dashboard. GAMES lists what can be installed and INSTALLED what's in
 * E:\Games; the other sections say they aren't here yet. Y searches GAMES with the on-screen
 * keyboard (keyboard.h). Plain C over ui_draw like the widgets, tested on the PC
 * (test_ui_library).
 */

#include <stdbool.h>

#include "input.h"
#include "keyboard.h"
#include "ui.h"
#include "widgets.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ui_lib_state {
    UI_LIB_NOT_TRIED,  /* not installed, and nothing started */
    UI_LIB_INSTALLED,  /* installed and verified, in this run or an earlier one */
    UI_LIB_FAILED,     /* tried in this run, and it failed */
    UI_LIB_STOPPED,    /* tried in this run, and the user stopped it */
    UI_LIB_UNFINISHED, /* started in an earlier run and not finished: installing carries on */
} ui_lib_state;

/* One game, as the caller describes it. The strings are the caller's and must stay alive. */
typedef struct ui_lib_item {
    const char *name;  /* the list's line and the details' heading: any UTF-8 */
    const char *facts; /* under the heading, small: year, region, size; may be "" */
    const char *about; /* wrapped below: who made it, the release, where it comes from */
    ui_lib_state state;
    const char *note; /* what happened, or where it's installed; may be NULL */
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
    UI_LIB_INSTALL, /* install items[ui_library_game()]: the user said yes */
    UI_LIB_EXIT,    /* back to the dashboard: the user said yes */
} ui_lib_action;

#define UI_LIB_QUERY_MAX 64 /* a search's size, with the NUL */

typedef struct ui_library {
    const ui_lib_item *items; /* GAMES: every game */
    int count;
    int *shown;   /* the caller's, room for count: the items GAMES lists, in order */
    ui_list list; /* over shown */
    char query[UI_LIB_QUERY_MAX];  /* what GAMES is searched for; "" lists every game */
    char typing[UI_LIB_QUERY_MAX]; /* the keyboard's, while it's open */
    bool searching;                /* the keyboard is open */
    ui_kbd kbd;
    const ui_lib_item *installed; /* INSTALLED: the folders in E:\Games */
    int installed_count;
    ui_list installed_list;
    ui_lib_section section;
    bool in_sections;     /* the D-pad moves between the sections, not the games */
    ui_lib_action asking; /* what the open dialog asks; UI_LIB_NONE: none is open */
    bool backdrop;        /* the screen behind the dialog or keyboard is saved */
    ui_dialog dialog;
    char message[256]; /* the dialog's */
} ui_library;

/* Starts the screen on the first game of GAMES, listing every game, with nothing in INSTALLED.
   `shown` has room for `count` ints and stays the library's while it's open. */
void ui_library_open(ui_library *lib, const ui_lib_item *items, int count, int *shown);

/* The index in items of the game highlighted in GAMES, or -1 if GAMES lists none. */
int ui_library_game(const ui_library *lib);

/* True if every word of `query` (split at spaces) is in the item's name, facts or about,
   ASCII case-insensitive. A query of only spaces, or "", matches everything. */
bool ui_library_matches(const ui_lib_item *it, const char *query);

/* Gives INSTALLED its list (again, after an install), keeping its place where it can. */
void ui_library_set_installed(ui_library *lib, const ui_lib_item *items, int count);

/* How many list lines the screen shows. */
int ui_library_rows(const ui_draw *d);

/* One button press. In a list, the D-pad, LT and RT move (ui_list_input), Left goes to the
   sections, A in GAMES asks to install the highlighted game, B in a searched GAMES lists every
   game again, and Back asks to leave. In the sections, Up and Down choose one, and Right or A go
   back to the list. Y, anywhere but a dialog, opens the keyboard on the search: DONE searches
   GAMES and shows it, keeping the highlighted game if it still matches; B there changes nothing.
   In a dialog or the keyboard, their buttons. */
ui_lib_action ui_library_input(ui_library *lib, ui_button b, int rows);

/* Draws the screen with the background at pan (ui_pan_x), and `status` (may be NULL) in the
   footer. Under an open dialog or the keyboard, the screen behind stands still (ui_backdrop). */
void ui_library_draw(const ui_draw *d, ui_library *lib, int pan, const char *status);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_UI_LIBRARY_H */
