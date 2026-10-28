#ifndef XCAT_UI_STARTUP_VIEW_H
#define XCAT_UI_STARTUP_VIEW_H

/*
 * The startup screen (M8, docs/ui-style.md#startup): what the app is doing before the library
 * opens, in place of nxdk's debug console (ADR 0015 decision 1). It shows the version and the
 * newest lines of what startup logs, oldest dimmed. If startup can't go on, it says why and how
 * to leave. Plain C over ui_draw, tested on the PC (test_ui_startup).
 */

#include <stdbool.h>

#include "ui.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_STARTUP_LINES 9      /* the newest lines kept, as many as the panel shows */
#define UI_STARTUP_LINE_MAX 160 /* a line's bytes, with the NUL; longer ones are cut */

typedef struct ui_startup_view {
    const char *version;                               /* "0.8.0": the caller's */
    char lines[UI_STARTUP_LINES][UI_STARTUP_LINE_MAX]; /* the oldest first */
    int count;
    bool failed; /* startup stopped: `why` says why */
    char why[320];
} ui_startup_view;

/* Starts the screen with no lines. */
void ui_startup_open(ui_startup_view *v, const char *version);

/* Adds what startup logged: each line of `text` that isn't blank, its ends trimmed, the oldest
   dropped when there are more than UI_STARTUP_LINES. The log's "==== ..." line between runs is
   left out: the screen's heading names the version. */
void ui_startup_add(ui_startup_view *v, const char *text);

/* Startup can't go on: the screen says so, with `why` (copied) and how to leave. */
void ui_startup_fail(ui_startup_view *v, const char *why);

/* Draws the screen with the background at pan. */
void ui_startup_draw(const ui_draw *d, const ui_startup_view *v, int pan);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_UI_STARTUP_VIEW_H */
