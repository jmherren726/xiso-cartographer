#ifndef XCAT_UI_SCRIPT_H
#define XCAT_UI_SCRIPT_H

/*
 * The UI script (docs/formats/ui-script.md, ADR 0015 decision 6): controller presses, waits and
 * screen checks that the app plays instead of a person, for the scripted xemu walkthrough. This
 * reads the file; the app plays it (app/ui/script.h) and knows the screens' names.
 *
 * The file is all-or-nothing (§5): a problem anywhere rejects it, and nothing is played.
 */

#include <stddef.h>
#include <stdint.h>

#include "xcat/error.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XC_UI_SCRIPT_MAX_FILE ((size_t)64 * 1024) /* §3.1 */
#define XC_UI_SCRIPT_MAX_LINE 256                 /* §3.1, bytes without the line ending */
#define XC_UI_SCRIPT_MAX_STEPS 512                /* §3.1, commands */
#define XC_UI_SCRIPT_MAX_PRESSES 100U             /* §4.1, a press's count */
#define XC_UI_SCRIPT_MAX_WAIT_MS 600000U          /* §4.2: ten minutes */
#define XC_UI_SCRIPT_EXPECT_MS 10000U             /* §4.3, the default */
#define XC_UI_SCRIPT_MAX_EXPECT_MS 3600000U       /* §4.3: an hour, for an install */
#define XC_UI_SCRIPT_SCREEN_MAX 31                /* §4.3, bytes of a screen's name */
#define XC_UI_SCRIPT_TEXT_SIZE 96                 /* §4.4, a note's bytes, with the NUL */
#define XC_UI_SCRIPT_VERSION_MAJOR 1

/* The controller's buttons, by their names in the file (§4.1). */
typedef enum xc_ui_key {
    XC_UI_KEY_UP,
    XC_UI_KEY_DOWN,
    XC_UI_KEY_LEFT,
    XC_UI_KEY_RIGHT,
    XC_UI_KEY_A,
    XC_UI_KEY_B,
    XC_UI_KEY_X,
    XC_UI_KEY_Y,
    XC_UI_KEY_START,
    XC_UI_KEY_BACK,
    XC_UI_KEY_LT,
    XC_UI_KEY_RT,
    XC_UI_KEY_WHITE,
    XC_UI_KEY_BLACK,
    XC_UI_KEY_COUNT_
} xc_ui_key;

typedef enum xc_ui_op {
    XC_UI_PRESS,  /* §4.1: press `key`, `count` times */
    XC_UI_WAIT,   /* §4.2: wait `ms` */
    XC_UI_EXPECT, /* §4.3: wait up to `ms` for the screen named `text` */
    XC_UI_NOTE,   /* §4.4: log `text` */
} xc_ui_op;

typedef struct xc_ui_step {
    xc_ui_op op;
    xc_ui_key key;
    uint32_t count;
    uint32_t ms;
    char text[XC_UI_SCRIPT_TEXT_SIZE]; /* NUL-terminated; "" for a press or a wait */
    size_t line;                       /* 1-based, in the file */
} xc_ui_step;

typedef struct xc_ui_script {
    xc_ui_step steps[XC_UI_SCRIPT_MAX_STEPS];
    size_t count;
} xc_ui_script;

/* Why a file was rejected. */
typedef struct xc_ui_script_error {
    size_t line; /* 1-based; 0 for the file as a whole */
    char message[128];
} xc_ui_script_error;

/*
 * Parses a script held in data[0, len). On XC_OK `s` holds its commands in order. On
 * XC_ERR_UI_SCRIPT `s` is empty and `err` says where and why.
 */
xc_err xc_ui_script_parse(const char *data, size_t len, xc_ui_script *s, xc_ui_script_error *err);

/* A button's name in the file ("a", "down", "white"), or "?" for an unknown value. */
const char *xc_ui_key_name(xc_ui_key key);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_UI_SCRIPT_H */
