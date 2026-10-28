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

/* The background pans at this speed, in pixels a second, and takes this long to slow to a stop
   at an end and as long to get back up to speed. */
#define UI_PAN_SPEED 20u
#define UI_PAN_EASE_MS 1000u

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
extern const ui_color ui_col_idle;             /* the selected line's fill, its list idle */
extern const ui_color ui_col_idle_border;      /* and its border */
extern const ui_color ui_col_shade;            /* over the screen behind a dialog or keyboard */

/* How far the background is panned at t_ms, in 0..range pixels: there and back, each way
   taking ui_pan_leg_ms(range), at UI_PAN_SPEED but for UI_PAN_EASE_MS at each end, where it
   slows evenly to a stop and speeds up again, so it never jumps. */
int ui_pan_x(uint32_t t_ms, int range);

/* How long the background takes to pan one way across range pixels (0 for none). */
uint32_t ui_pan_leg_ms(int range);

/* How far the background can pan: its width less the screen's (0 without an image). */
int ui_pan_range(const ui_draw *d);

/* Fills the screen with the background panned to x (from ui_pan_x), or ui_col_bg. Panels in
   ui_col_panel drawn after it, until ui_backdrop_save or ui_backdrop, are copied from
   UI_IMAGE_BACKGROUND_PANEL at the same pan when the backend has it: the same pixels as
   blending, at the cost of a copy. */
void ui_background(const ui_draw *d, int x);

/* At most this many panels for ui_background_under; more are drawn over. */
#define UI_BACKGROUND_PANELS 16

/* ui_background, but leaving out the inside of `panels` (n of them), which the screen must
   then draw with ui_panel in ui_col_panel, with a chamfer up to UI_CHAMFER: they cover it
   with their copy of the tinted background, and on the console every pixel copied costs
   (docs/ui-style.md#cost). Without the tinted background, the panels are blended over the
   whole background, as ui_background. */
void ui_background_under(const ui_draw *d, int x, const ui_rect *panels, int n);

/* A panel: `fill` inside a UI_BORDER border, with the top-left and bottom-right corners cut
   at 45 degrees by `chamfer` pixels (0 for square corners). */
void ui_panel(const ui_draw *d, ui_rect r, int chamfer, ui_color fill, ui_color border);

/* Behind a dialog or the keyboard: ui_backdrop_save keeps the screen drawn so far, darkened by
   ui_col_shade, and draws it, in the frame the dialog opens; ui_backdrop draws it again in the
   frames after, instead of the whole screen. The screen behind stands still meanwhile, and a
   frame costs a copy and the dialog. Panels drawn after either are blended. */
void ui_backdrop_save(const ui_draw *d);
void ui_backdrop(const ui_draw *d);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_UI_THEME_H */
