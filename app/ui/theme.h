#ifndef XCAT_UI_THEME_H
#define XCAT_UI_THEME_H

/*
 * The look of every screen (docs/ui-style.md): the colours, the panning background, and
 * panels with cut corners. Drawn through ui_draw only, like the rest of app/ui (ADR 0015).
 */

#include <stdint.h>

#include "ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The background takes this long to pan from one end to the other, and as long back. */
#define UI_PAN_LEG_MS 60000u

/* The size of a panel's cut corners, and the width of its border, in pixels. */
#define UI_CHAMFER 12
#define UI_BORDER 2

extern const ui_color ui_col_bg;               /* behind everything if there's no image */
extern const ui_color ui_col_text;             /* body text */
extern const ui_color ui_col_dim;              /* hints, details */
extern const ui_color ui_col_accent;           /* titles */
extern const ui_color ui_col_panel;            /* panel fill, translucent */
extern const ui_color ui_col_border;           /* panel border */
extern const ui_color ui_col_highlight;        /* the selected line's fill */
extern const ui_color ui_col_highlight_border; /* the selected line's border */

/* How far the background is panned at t_ms, in 0..range pixels: there and back every
   2 * UI_PAN_LEG_MS, easing in and out at the ends (smoothstep), so it never jumps. */
int ui_pan_x(uint32_t t_ms, int range);

/* How far the background can pan: its width less the screen's (0 without an image). */
int ui_pan_range(const ui_draw *d);

/* Fills the screen with the background panned to x (from ui_pan_x), or ui_col_bg. */
void ui_background(const ui_draw *d, int x);

/* A panel: `fill` inside a UI_BORDER border, with the top-left and bottom-right corners cut
   at 45 degrees by `chamfer` pixels (0 for square corners). */
void ui_panel(const ui_draw *d, ui_rect r, int chamfer, ui_color fill, ui_color border);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_UI_THEME_H */
