# The UI's look

How the M8 screens look ([ADR 0015](adr/0015-ui-sdl2-own-widgets.md) decides how they're built). The code is [`app/ui/theme.h`](../app/ui/theme.h) and [`theme.c`](../app/ui/theme.c); [`app/ui-test`](../app/ui-test/main.c) shows all of it on one screen.

## Where it comes from

The owner's mockups of 2026-10-21: a library screen and a download screen in the style of the original Xbox dashboard, with dark green-black panels, bright lime borders and cut corners. The mockups aren't in the repo, since they show game screenshots and the console maker's logo; this page describes what's taken from them.

Owner decisions of 2026-10-21:

- **The background is the owner's own panorama**, panning slowly from one end to the other and back ([below](#the-background)). The owner made it and released it with the project under its MIT licence ([`app/ui/assets/README.md`](../app/ui/assets/README.md)).
- **No cover art or screenshots for now.** The screens follow the mockups' layout without thumbnails; the detail panel shows text from the catalog (title, year, region, size, the Redump name). Art needs its own source and licence decision, in a later milestone, as M7 decided too ([ADR 0014](adr/0014-catalog-sources-and-format.md)).
- **No logos or marks of the console maker.**

## Colours

| Name (`ui_col_…`) | RGBA | Use |
|---|---|---|
| `bg` | 4, 14, 4 | Behind everything if the background image is missing |
| `text` | 214, 236, 204 | Body text, the selected line |
| `dim` | 134, 172, 122 | Hints, details, sections not selected |
| `accent` | 150, 240, 80 | Titles, the selected game's name |
| `panel` | 6, 22, 8, alpha 210 | Panel fill: the background shows through a little |
| `border` | 80, 190, 40 | Panel borders |
| `highlight` | 40, 110, 20, alpha 235 | The selected line or section |
| `highlight_border` | 160, 255, 70 | Its border |

Text is Noto Sans, the bundled font (ADR 0015 decision 7): Bold 28 for titles, Bold 20 for the selected line and section names, Regular 20 for list lines, Regular 16 for details and hints.

## Panels

`ui_panel(d, rect, chamfer, fill, border)`: a translucent fill inside a 2-pixel border, with the **top-left and bottom-right corners cut** at 45° (`UI_CHAMFER`, 12 pixels, for screen panels; 6–8 for rows and buttons inside them; 0 for the footer). It's drawn with fills only, a row at a time across the cut corners, and the fill never paints a pixel twice, which would darken it unevenly.

A selected row or section is a small panel in `highlight` / `highlight_border`, with its text in Bold.

## Layout

All inside the title-safe area (32, 24, 576 × 432), as in ui-test and the library mockup:

| Part | Rectangle | Holds |
|---|---|---|
| Title tab | 32, 24, 240 × 46 | The screen's name in capitals, Bold 28, `accent` |
| Sections | 32, 80, 170 × 196 | GAMES, DOWNLOADS, INSTALLED, SETTINGS |
| List | 210, 80, 212 × 320 | The games, one row per line, the selected one highlighted |
| Detail | 430, 80, 178 × 320 | The selected game: name, details, description |
| Footer | 32, 408, 576 × 48 | Status and the buttons' meanings |

## The background

`app/ui/assets/background.png` is 1440 × 480: the screen's height, and 800 pixels wider than the screen. The XBE build converts it to `ui\background.bmp` next to the XBE (`tools/ui/png2bmp.py`, since nxdk's SDL loads BMP without SDL2_image), and the backend loads it at start. Without it, the screens draw on `ui_col_bg`.

`ui_pan_x(t)` gives the offset: 60 seconds (`UI_PAN_LEG_MS`) from the left end to the right, and as long back, easing in and out at the ends (smoothstep), so it never jumps or stops abruptly. At its fastest it moves 20 pixels a second.

**Cost.** Every frame is drawn by the CPU, and copied into the framebuffer that isn't shown (ADR 0015, `draw_sdl.c`). So a screen draws a new frame only when something changed: a button, new text, or the background moving a whole pixel. With the pan that's up to 20 frames a second, slower near the ends. In xemu a frame of the ui-test screen takes 2–3 ms to draw, about 5 % of the CPU at 20 frames a second. The install screens keep the same rule; if the pan costs installs speed, they can stop it while a download runs (to be measured on hardware in the M8 install screen).
