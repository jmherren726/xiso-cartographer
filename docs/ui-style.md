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

## Fonts

Three bundled fonts (ADR 0015 decision 7, [third_party/fonts](../third_party/fonts/README.md)), all SIL OFL 1.1:

| `ui_font` | Font | Size | Use |
|---|---|---|---|
| `UI_FONT_TITLE` | Michroma | 22 | Screen titles in the title tab, in capitals |
| `UI_FONT_LABEL` | Chakra Petch Bold | 21 | Section names and labels, in capitals |
| `UI_FONT_BOLD` | Noto Sans Bold | 20 | The selected line, the selected game's name |
| `UI_FONT_BODY` | Noto Sans Regular | 20 | List lines, text |
| `UI_FONT_SMALL` | Noto Sans Regular | 16 | Details and hints |

Michroma is wide and geometric like the mockups' titles; Chakra Petch's cut corners match the panels'. Both lack Greek and Cyrillic, so they're for the app's own fixed words only: **anything from outside the app (game names, the catalog, error details) is drawn in Noto Sans.** Picked on 2026-10-21 from a comparison of static OFL fonts (Michroma, Chakra Petch, Audiowide, Share Tech; variable fonts such as Orbitron and Oxanium were out, since nxdk's FreeType 2.4.12 predates them).

## Panels

`ui_panel(d, rect, chamfer, fill, border)`: a translucent fill inside a 2-pixel border, with the **top-left and bottom-right corners cut** at 45° (`UI_CHAMFER`, 12 pixels, for screen panels; 6–8 for rows and buttons inside them; 0 for the footer). It's drawn with fills only, a row at a time across the cut corners, and the fill never paints a pixel twice, which would darken it unevenly.

A selected row or section is a small panel in `highlight` / `highlight_border`, with its text in Bold.

## Layout

All inside the title-safe area (32, 24, 576 × 432), as in ui-test and the library mockup:

| Part | Rectangle | Holds |
|---|---|---|
| Title tab | 32, 24, 240 × 46 | The screen's name in capitals, `UI_FONT_TITLE`, `accent` |
| Sections | 32, 80, 170 × 196 | GAMES, DOWNLOADS, INSTALLED, SETTINGS |
| List | 210, 80, 212 × 320 | The games, one row per line, the selected one highlighted |
| Detail | 430, 80, 178 × 320 | The selected game: name, details, description |
| Footer | 32, 408, 576 × 48 | Status and the buttons' meanings |

## Widgets

The widgets of ADR 0015 decisions 2 and 3, in [`app/ui/widgets.h`](../app/ui/widgets.h) and [`keyboard.h`](../app/ui/keyboard.h). Each is a plain struct the screen owns: `*_input` changes it for one button press, `*_draw` draws it, and both are tested on the PC. ui-test shows them all ([testing.md](testing.md#m8-ui-test-rendering)).

**Buttons** ([`input.h`](../app/ui/input.h)). A chooses, B goes back, Y opens search and Back leaves the app, on every screen. A held D-pad direction presses again after 400 ms, then every 80 ms; no other button repeats, so holding A never chooses twice. The triggers count as pressed past half way and are ready again below a quarter. `input_sdl.c` reads every plugged-in controller.

**List.** One line per item in `UI_FONT_BODY`, 6 pixels apart, cut with "…" when too wide; the highlighted line is a small panel (`highlight`, chamfer 6) in `UI_FONT_BOLD`. Up and Down move a line, LT and RT a page, and the highlight stops at the ends. When not all lines fit, a 4-pixel scroll bar runs down the right edge: the track in `highlight`, the thumb in `highlight_border`.

**Progress bar.** A small panel (chamfer 6) filled in `highlight` from the left, with its label (say "48 %") centred on it in `UI_FONT_BOLD`. `ui_percent` rounds down, so it shows 100 % only once the last byte is in.

**Dialog.** The screen dimmed (black at alpha 150), then a 520-pixel panel centred on it: the title in `UI_FONT_LABEL` and `accent`, the message wrapped in `UI_FONT_BODY` (at most six lines), and one to three buttons side by side in `UI_FONT_LABEL`, the highlighted one in `highlight`. Left and Right move between them; A chooses; B gives the choice marked as cancel, if any. Error dialogs (ADR 0015 decision 5) will use it with RETRY, LOG IN AGAIN and BACK, which fit.

**On-screen keyboard.** The dimmed screen, then a 520-pixel panel: the field's name in `UI_FONT_LABEL`, the field, 10 × 5 keys and a line of hints.

| Row | Small letters | Capitals | Symbols |
|---|---|---|---|
| 1 | `1234567890` | the same | the same |
| 2 | `qwertyuiop` | `QWERTYUIOP` | `!?#$%^&*()` |
| 3 | `asdfghjkl-` | `ASDFGHJKL-` | `` ~`+=[]{}<> `` |
| 4 | `zxcvbnm.@_` | `ZXCVBNM.@_` | backslash, pipe, then `/;:'",-_` |
| 5 | SHIFT, #+= (ABC on the symbols), SPACE, DELETE, DONE: two columns each | | |

Between them the three layers type every printable ASCII character, so any password can be typed. The D-pad moves and wraps around the edges; on the bottom row it goes key by key. A types the highlighted key, X deletes the last character, Y adds a space, LT switches capitals on and off and RT the symbols (the SHIFT and #+= keys do the same), Start or DONE finishes, B cancels. SHIFT is drawn in `accent` while capitals are on.

The field shows the end of its text when it's wider, with an `accent` caret after it. The keyboard edits the caller's buffer in place, up to its size, and keeps no copy. **A password field shows one dot (U+2022) per character, and its text never reaches the drawing backend**, not even to be measured, so it never sits in a texture or the text cache; the caller wipes the buffer once it's used (ADR 0010 decision 2). A test checks that the backend never sees it.

## The background

`app/ui/assets/background.png` is 1440 × 480: the screen's height, and 800 pixels wider than the screen. The XBE build converts it to `ui\background.bmp` next to the XBE (`tools/ui/png2bmp.py`, since nxdk's SDL loads BMP without SDL2_image), and the backend loads it at start. Without it, the screens draw on `ui_col_bg`.

`ui_pan_x(t)` gives the offset: 60 seconds (`UI_PAN_LEG_MS`) from the left end to the right, and as long back, easing in and out at the ends (smoothstep), so it never jumps or stops abruptly. At its fastest it moves 20 pixels a second.

**Cost.** Every frame is drawn by the CPU, and copied into the framebuffer that isn't shown (ADR 0015, `draw_sdl.c`). So a screen draws a new frame only when something changed: a button, new text, or the background moving a whole pixel. With the pan that's up to 20 frames a second, slower near the ends. In xemu a frame of the ui-test screen takes 2–3 ms to draw, about 5 % of the CPU at 20 frames a second. The install screens keep the same rule; if the pan costs installs speed, they can stop it while a download runs (to be measured on hardware in the M8 install screen).
