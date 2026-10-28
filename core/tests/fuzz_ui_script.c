/*
 * libFuzzer target for the UI script (docs/formats/ui-script.md).
 *
 * The input is a script to parse. It gives XC_OK with every command within its limits, or
 * XC_ERR_UI_SCRIPT with no commands and a line inside the file. A violation aborts. Seeds are
 * in core/tests/fuzz-seeds/ui-script/.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "xcat/ui_script.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void check(int ok)
{
    if (!ok) {
        abort();
    }
}

static xc_ui_script script;

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    xc_ui_script_error e;
    const char *text = size > 0 ? (const char *)data : NULL;
    xc_err err = xc_ui_script_parse(text, size, &script, &e);
    check(err == XC_OK || err == XC_ERR_UI_SCRIPT);
    check(memchr(e.message, '\0', sizeof e.message) != NULL);
    size_t lines = 1;
    for (size_t i = 0; i < size; i++) {
        lines += data[i] == '\n' ? 1U : 0U;
    }
    if (err != XC_OK) {
        check(script.count == 0);
        check(e.line <= lines);
        return 0;
    }
    check(script.count <= XC_UI_SCRIPT_MAX_STEPS);
    size_t last = 0;
    for (size_t i = 0; i < script.count; i++) {
        const xc_ui_step *st = &script.steps[i];
        check(st->line > last && st->line <= lines);
        last = st->line;
        check(memchr(st->text, '\0', sizeof st->text) != NULL);
        switch (st->op) {
        case XC_UI_PRESS:
            check(st->key < XC_UI_KEY_COUNT_);
            check(st->count >= 1 && st->count <= XC_UI_SCRIPT_MAX_PRESSES);
            break;
        case XC_UI_WAIT:
            check(st->ms <= XC_UI_SCRIPT_MAX_WAIT_MS);
            break;
        case XC_UI_EXPECT:
            check(st->ms >= 1 && st->ms <= XC_UI_SCRIPT_MAX_EXPECT_MS);
            check(st->text[0] != '\0' && strlen(st->text) <= XC_UI_SCRIPT_SCREEN_MAX);
            break;
        case XC_UI_NOTE:
            check(st->text[0] != '\0');
            break;
        default:
            check(0);
        }
    }
    return 0;
}
