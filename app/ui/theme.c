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

/* The time spent easing at each end of a leg: UI_PAN_EASE_MS, or less when the range is too
   short to reach full speed. */
static uint64_t pan_ease_ms(int range)
{
    uint64_t cruise = (uint64_t)range * 1000U / UI_PAN_SPEED;
    return cruise < UI_PAN_EASE_MS ? cruise : UI_PAN_EASE_MS;
}

uint32_t ui_pan_leg_ms(int range)
{
    if (range <= 0) {
        return 0;
    }
    /* Easing in and out covers as much ground as full speed would in one ease. */
    return (uint32_t)((uint64_t)range * 1000U / UI_PAN_SPEED + pan_ease_ms(range));
}

int ui_pan_x(uint32_t t_ms, int range)
{
    uint64_t leg = ui_pan_leg_ms(range);
    if (leg == 0) {
        return 0;
    }
    uint64_t ease = pan_ease_ms(range);
    uint64_t in_cycle = t_ms % (2 * leg);
    uint64_t u = in_cycle % leg; /* 0..leg, into this leg */
    /* Speeding up evenly over `ease`, then at full speed, then slowing evenly: the distance
       in pixels is speed * time, and while easing speed * t^2 / (2 * ease). */
    uint64_t x;
    if (ease > 0 && u < ease) {
        x = UI_PAN_SPEED * u * u / (2000U * ease);
    } else if (ease > 0 && leg - u < ease) {
        x = (uint64_t)range - UI_PAN_SPEED * (leg - u) * (leg - u) / (2000U * ease);
    } else {
        x = UI_PAN_SPEED * (2 * u - ease) / 2000U;
    }
    if (x > (uint64_t)range) {
        x = (uint64_t)range;
    }
    return in_cycle < leg ? (int)x : range - (int)x;
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
    ui_background_under(d, x, NULL, 0);
}

/* Where panel p leaves the background out: its rows clear of any cut corner. */
static ui_rect hole(ui_rect p)
{
    return (ui_rect){p.x, p.y + UI_CHAMFER, p.w, p.h - 2 * UI_CHAMFER};
}

/* Copies the background at pan x to the rows [y0, y1) of the screen, but for the holes
   that cover them. */
static void background_band(const ui_draw *d, int x, int y0, int y1, const ui_rect *holes, int n)
{
    int from = 0;
    while (from < UI_SCREEN_W) {
        /* The first hole in this band at or after `from`, or the screen's edge. */
        int to = UI_SCREEN_W;
        int next = UI_SCREEN_W;
        for (int i = 0; i < n; i++) {
            const ui_rect *h = &holes[i];
            if (h->y <= y0 && h->y + h->h >= y1 && h->x + h->w > from && h->x < to) {
                to = h->x > from ? h->x : from;
                next = h->x + h->w;
            }
        }
        if (to > from) {
            d->image(d->ctx, UI_IMAGE_BACKGROUND, (ui_rect){x + from, y0, to - from, y1 - y0}, from,
                     y0);
        }
        from = to == UI_SCREEN_W ? UI_SCREEN_W : next;
    }
}

void ui_background_under(const ui_draw *d, int x, const ui_rect *panels, int n)
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
    int tw = 0;
    int th = 0;
    d->image_size(d->ctx, UI_IMAGE_BACKGROUND_PANEL, &tw, &th);
    if (tw == w && th == h) {
        tint_x = x;
    } else {
        n = 0; /* the panels will be blended, over the background */
    }
    /* The holes, and the rows where one starts or ends: between two of those, the same
       holes cover the whole band. */
    ui_rect holes[UI_BACKGROUND_PANELS];
    int ys[2 * UI_BACKGROUND_PANELS + 2];
    int nh = 0;
    int ny = 0;
    ys[ny++] = 0;
    ys[ny++] = UI_SCREEN_H;
    for (int i = 0; i < n && nh < UI_BACKGROUND_PANELS; i++) {
        ui_rect r = hole(panels[i]);
        if (r.w > 0 && r.h > 0) {
            holes[nh++] = r;
            ys[ny++] = r.y;
            ys[ny++] = r.y + r.h;
        }
    }
    for (int i = 1; i < ny; i++) { /* sort the rows: a handful */
        for (int j = i; j > 0 && ys[j - 1] > ys[j]; j--) {
            int t = ys[j];
            ys[j] = ys[j - 1];
            ys[j - 1] = t;
        }
    }
    for (int i = 0; i + 1 < ny; i++) {
        int y0 = ys[i] < 0 ? 0 : ys[i];
        int y1 = ys[i + 1] > UI_SCREEN_H ? UI_SCREEN_H : ys[i + 1];
        if (y1 > y0) {
            background_band(d, x, y0, y1, holes, nh);
        }
    }
}

void ui_backdrop_save(const ui_draw *d)
{
    d->save_backdrop(d->ctx, ui_col_shade);
    d->backdrop(d->ctx); /* the screen saved is not yet darkened in this frame */
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
