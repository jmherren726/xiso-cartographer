/* The startup screen (startup_view.h). */
#include "startup_view.h"

#include <stdio.h>
#include <string.h>

#include "theme.h"

#define PAD 14

static const ui_rect tab = {32, 24, 240, 46};
static const ui_rect body = {32, 80, 576, 320};
static const ui_rect footer = {32, 408, 576, 48};

void ui_startup_open(ui_startup_view *v, const char *version)
{
    memset(v, 0, sizeof *v);
    v->version = version;
}

static void add_line(ui_startup_view *v, const char *s, size_t n)
{
    while (n > 0 && (*s == ' ' || *s == '\t' || *s == '\r')) {
        s++;
        n--;
    }
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) {
        n--;
    }
    if (n == 0 || (n >= 4 && memcmp(s, "====", 4) == 0)) {
        return; /* the log's line between runs: the heading says the same */
    }
    if (v->count == UI_STARTUP_LINES) {
        memmove(v->lines[0], v->lines[1], sizeof v->lines - sizeof v->lines[0]);
        v->count--;
    }
    if (n >= UI_STARTUP_LINE_MAX) {
        n = UI_STARTUP_LINE_MAX - 1; /* the drawing cuts it to the panel anyway */
        while (n > 0 && ((unsigned char)s[n] & 0xC0U) == 0x80U) {
            n--; /* not inside a UTF-8 sequence */
        }
    }
    memcpy(v->lines[v->count], s, n);
    v->lines[v->count][n] = '\0';
    v->count++;
}

void ui_startup_add(ui_startup_view *v, const char *text)
{
    while (*text != '\0') {
        size_t n = strcspn(text, "\n");
        add_line(v, text, n);
        text += n;
        if (*text == '\n') {
            text++;
        }
    }
}

void ui_startup_fail(ui_startup_view *v, const char *why)
{
    (void)snprintf(v->why, sizeof v->why, "%s", why != NULL ? why : "");
    v->failed = true;
}

void ui_startup_draw(const ui_draw *d, const ui_startup_view *v, int pan)
{
    const ui_rect panels[] = {tab, body, footer};
    ui_background_under(d, pan, panels, (int)(sizeof panels / sizeof panels[0]));

    ui_panel(d, tab, UI_CHAMFER, ui_col_panel, ui_col_border);
    int th = d->line_height(d->ctx, UI_FONT_TITLE);
    (void)ui_text_fit(d, UI_FONT_TITLE, tab.x + 22, tab.y + (tab.h - th) / 2, tab.w - 30,
                      ui_col_accent, v->failed ? "STOPPED" : "STARTING");

    ui_panel(d, body, UI_CHAMFER, ui_col_panel, ui_col_border);
    int x = body.x + PAD + 6;
    int w = body.w - 2 * PAD - 12;
    int y = body.y + PAD;
    int bh = d->line_height(d->ctx, UI_FONT_BOLD);
    int sh = d->line_height(d->ctx, UI_FONT_SMALL);
    char head[64];
    (void)snprintf(head, sizeof head, "xiso-cartographer %s", v->version != NULL ? v->version : "");
    (void)ui_text_fit(d, UI_FONT_BOLD, x, y, w, ui_col_accent, head);
    y += bh + 6;
    if (v->failed) {
        (void)ui_text_fit(d, UI_FONT_LABEL, x, y, w, ui_col_text, "CAN'T GO ON");
        y += d->line_height(d->ctx, UI_FONT_LABEL);
        y += ui_text_wrap(d, UI_FONT_BODY, (ui_rect){x, y, w, 4 * bh}, ui_col_text, v->why) *
             d->line_height(d->ctx, UI_FONT_BODY);
        y += 8;
    }
    /* The newest lines that fit, the newest last in `text`, the ones before in `dim`. */
    int bottom = body.y + body.h - PAD;
    int fit = (bottom - y) / sh;
    int first = v->count > fit ? v->count - fit : 0;
    for (int i = first; i < v->count; i++) {
        bool newest = i == v->count - 1 && !v->failed;
        (void)ui_text_fit(d, UI_FONT_SMALL, x, y, w, newest ? ui_col_text : ui_col_dim,
                          v->lines[i]);
        y += sh;
    }

    ui_panel(d, footer, 0, ui_col_panel, ui_col_border);
    int by = footer.y + (footer.h - sh) / 2;
    (void)ui_text_fit(d, UI_FONT_SMALL, footer.x + 12, by, footer.w - 24, ui_col_dim,
                      "Hold LT + RT + Back + Black to return to the dashboard");
}
