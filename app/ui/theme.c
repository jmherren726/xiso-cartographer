/* The look of every screen (theme.h, docs/ui-style.md). */
#include "theme.h"

#include <stdbool.h>

/* Greens after the owner's mockups (docs/ui-style.md). */
const ui_color ui_col_bg = {4, 14, 4, 255};
const ui_color ui_col_text = {214, 236, 204, 255};
const ui_color ui_col_dim = {134, 172, 122, 255};
const ui_color ui_col_accent = {150, 240, 80, 255};
const ui_color ui_col_panel = {6, 22, 8, 210};
const ui_color ui_col_border = {80, 190, 40, 255};
const ui_color ui_col_highlight = {40, 110, 20, 235};
const ui_color ui_col_highlight_border = {160, 255, 70, 255};
const ui_color ui_col_shade = {0, 0, 0, 150};

/* Where ui_background took the background from, for the panels drawn after it; -1 while
   panels are blended (no background image, or over a backdrop). */
static int tint_x = -1;

static bool same_color(ui_color a, ui_color b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

int ui_pan_x(uint32_t t_ms, int range)
{
    if (range <= 0) {
        return 0;
    }
    const uint64_t leg = UI_PAN_LEG_MS;
    uint32_t in_cycle = t_ms % (2 * UI_PAN_LEG_MS);
    uint64_t u = in_cycle % UI_PAN_LEG_MS; /* 0..leg, into this leg */
    /* smoothstep: range * p^2 * (3 - 2p) with p = u / leg, in integers (fits 64 bits for
       ranges up to millions of pixels). */
    uint64_t x = (uint64_t)range * u * u * (3 * leg - 2 * u) / (leg * leg * leg);
    return in_cycle < UI_PAN_LEG_MS ? (int)x : range - (int)x;
}

int ui_pan_range(const ui_draw *d)
{
    int w = 0;
    int h = 0;
    d->image_size(d->ctx, UI_IMAGE_BACKGROUND, &w, &h);
    return w > UI_SCREEN_W && h >= UI_SCREEN_H ? w - UI_SCREEN_W : 0;
}

void ui_background(const ui_draw *d, int x)
{
    int w = 0;
    int h = 0;
    d->image_size(d->ctx, UI_IMAGE_BACKGROUND, &w, &h);
    tint_x = -1;
    if (w < UI_SCREEN_W || h < UI_SCREEN_H) {
        d->fill(d->ctx, (ui_rect){0, 0, UI_SCREEN_W, UI_SCREEN_H}, ui_col_bg);
        return;
    }
    if (x < 0) {
        x = 0;
    } else if (x > w - UI_SCREEN_W) {
        x = w - UI_SCREEN_W;
    }
    d->image(d->ctx, UI_IMAGE_BACKGROUND, (ui_rect){x, 0, UI_SCREEN_W, UI_SCREEN_H}, 0, 0);
    int tw = 0;
    int th = 0;
    d->image_size(d->ctx, UI_IMAGE_BACKGROUND_PANEL, &tw, &th);
    if (tw == w && th == h) {
        tint_x = x;
    }
}

void ui_backdrop_save(const ui_draw *d)
{
    d->save_backdrop(d->ctx, ui_col_shade);
    tint_x = -1;
}

void ui_backdrop(const ui_draw *d)
{
    d->backdrop(d->ctx);
    tint_x = -1;
}

/* Part of a panel's fill: copied from the tinted background, or blended. */
static void panel_fill(const ui_draw *d, ui_rect r, ui_color fill, bool tinted)
{
    if (r.w <= 0 || r.h <= 0) {
        return;
    }
    if (tinted) {
        d->image(d->ctx, UI_IMAGE_BACKGROUND_PANEL, (ui_rect){r.x + tint_x, r.y, r.w, r.h}, r.x,
                 r.y);
    } else {
        d->fill(d->ctx, r, fill);
    }
}

void ui_panel(const ui_draw *d, ui_rect r, int chamfer, ui_color fill, ui_color border)
{
    int c = chamfer;
    if (c > r.w / 2) {
        c = r.w / 2;
    }
    if (c > r.h / 2) {
        c = r.h / 2;
    }
    if (c < 0) {
        c = 0;
    }
    /* The fill: one row per pixel of each cut corner, one rectangle between them. */
    bool tinted = tint_x >= 0 && same_color(fill, ui_col_panel);
    for (int y = 0; y < c; y++) {
        int in = c - y;
        panel_fill(d, (ui_rect){r.x + in, r.y + y, r.w - in, 1}, fill, tinted);
    }
    panel_fill(d, (ui_rect){r.x, r.y + c, r.w, r.h - 2 * c}, fill, tinted);
    for (int k = 1; k <= c; k++) {
        panel_fill(d, (ui_rect){r.x, r.y + r.h - c + k - 1, r.w - k, 1}, fill, tinted);
    }
    /* The border: four straight edges, then the diagonals a row at a time. */
    d->fill(d->ctx, (ui_rect){r.x + c, r.y, r.w - c, UI_BORDER}, border);
    d->fill(d->ctx, (ui_rect){r.x, r.y + r.h - UI_BORDER, r.w - c, UI_BORDER}, border);
    d->fill(d->ctx, (ui_rect){r.x, r.y + c, UI_BORDER, r.h - c}, border);
    d->fill(d->ctx, (ui_rect){r.x + r.w - UI_BORDER, r.y, UI_BORDER, r.h - c}, border);
    for (int y = 0; y < c; y++) {
        d->fill(d->ctx, (ui_rect){r.x + c - y, r.y + y, UI_BORDER, 1}, border);
        d->fill(d->ctx, (ui_rect){r.x + r.w - (y + 1) - UI_BORDER, r.y + r.h - c + y, UI_BORDER, 1},
                border);
    }
}
