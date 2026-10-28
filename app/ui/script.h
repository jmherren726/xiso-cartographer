#ifndef XCAT_UI_SCRIPT_RUN_H
#define XCAT_UI_SCRIPT_RUN_H

/*
 * Plays a UI script (docs/formats/ui-script.md, ADR 0015 decision 6) as if a person held the
 * controller: its presses come out as ui_buttons, a gap apart, and each `expect` waits for the
 * screen it names. The first `expect` that times out stops the script. The controller keeps
 * working throughout, so a script can't lock anyone out. Portable, tested on the PC
 * (test_ui_script_run); xcat/ui_script.h reads the file.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "input.h"
#include "xcat/ui_script.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_SCRIPT_GAP_MS 300U /* §4.1: before each press, so the screen shows the last one */

typedef enum ui_script_state {
    UI_SCRIPT_OFF, /* no script */
    UI_SCRIPT_RUNNING,
    UI_SCRIPT_PASSED, /* every command ran, every expect saw its screen */
    UI_SCRIPT_FAILED, /* an expect timed out: the script stopped there */
} ui_script_state;

typedef struct ui_script {
    const xc_ui_script *s; /* the caller's, while it runs */
    ui_script_state state;
    size_t next;      /* the command being run */
    bool started;     /* its timer is set */
    uint32_t at_ms;   /* a press's or a wait's time, an expect's deadline */
    uint32_t left;    /* presses of it still to come */
    uint32_t checks;  /* expects that saw their screen */
    char status[160]; /* for the footer: where it is, or how it ended */
} ui_script;

/* True if `name` is a screen the app has (§6). */
bool ui_script_screen_known(const char *name);

/* NULL if every expect of s names a known screen, else why not, with its line, in msg. */
const char *ui_script_check(const xc_ui_script *s, char *msg, size_t size);

/* Starts playing s (which stays the caller's). */
void ui_script_start(ui_script *r, const xc_ui_script *s);

/*
 * One look at the script at now_ms, with the screen showing `screen`: the press due now, or
 * UI_BTN_NONE. A line for the log (a note, an expect met or missed, the end) is written to
 * `log`, else it's "". Times may wrap around 2^32 ms.
 */
ui_button ui_script_next(ui_script *r, uint32_t now_ms, const char *screen, char *log,
                         size_t log_size);

/* The ui_button a script's button stands for. */
ui_button ui_script_button(xc_ui_key key);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_UI_SCRIPT_RUN_H */
