# 0015. The UI: SDL2 and SDL_ttf, with a small controller-first widget set of our own

- **Status:** Accepted (owner decision, 2026-10-20)
- **Date:** 2026-10-20
- **Resolves:** HANDOFF §8 item 4 ("UI approach, ADR before M8")

## Context

M8 replaces the debug-console text list with the real UI: browse, search, detail, install, progress, settings, and the on-box login of [ADR 0010](0010-on-box-login-required-for-beta.md), which needs text entry on an on-screen keyboard. Its exit test asks for a scripted xemu walkthrough and a user-visible message for every error path ([HANDOFF §4.1](../HANDOFF.md#41-roadmap)).

What the console and nxdk give us:

- **Input:** a controller only. There is no mouse, and almost nobody has a USB keyboard on the console. The app already reads the controller through nxdk's SDL2 (`NXDK_SDL = y`, `SDL_GameController`).
- **Output:** a TV, usually at 640×480, with parts of the edges hidden (overscan).
- **nxdk's SDL2** has a renderer (`SDL_CreateRenderer`), and nxdk also builds **SDL_ttf** (with FreeType) and **SDL2_image**. Its `sdl_ttf` sample draws TrueType text this way. All are zlib-licensed ([THIRD_PARTY.md](../../THIRD_PARTY.md)).
- **pbkit** drives the GPU directly. It is fast, but it has no text or widget layer.
- **Immediate-mode libraries** in C (Nuklear, microui) are built around a mouse pointer. Focus moved by a D-pad, and a keyboard laid out for a controller, would have to be written on top of them anyway.

Options considered: SDL2 with our own widgets; SDL2 with Nuklear; pbkit.

## Decision

1. **SDL2 and SDL_ttf, as nxdk builds them.** One 640×480 window and one `SDL_Renderer`. Text is drawn with SDL_ttf, and glyphs are cached as textures so a frame doesn't rasterise text again. Nothing draws through the debug console anymore, apart from a fatal startup error.
2. **Our own small widget set, built for the controller.** It lives in `app/ui/` and has:
   - a focusable list that scrolls and pages, with a highlighted line;
   - text blocks that wrap;
   - a progress bar;
   - a dialog with up to three choices;
   - an on-screen keyboard.

   Every screen is drawn inside a title-safe area (a 5 % margin on each side). The buttons have one meaning throughout: **A** chooses, **B** goes back, **Y** opens search, and **Back** leaves the app. LT + RT + Back + Black still returns to the dashboard at any time.
3. **The on-screen keyboard** is a grid of letters, digits and symbols, with a shift and a symbols layer. The D-pad moves, A types, X deletes, Y adds a space, and Start finishes; B cancels. It writes into a buffer the caller owns, with a length limit. In a **password field** it shows dots, and the caller wipes the buffer after use; the keyboard keeps no copy (ADR 0010 decision 2). Nothing the keyboard sees is logged.
4. **The screen logic is plain C, apart from drawing.** Each screen's state, the keyboard and the list's navigation are written against a small drawing interface. They include no SDL or nxdk header, so they also build on the PC, with Unity tests in the host build. Only `app/ui/` files whose names end in `_sdl.c` include SDL. Parsers still live in `core/`; the UI only shows what core returns.
5. **Every error is shown.** Each `xc_err` already has a readable message (`xc_err_str`). The UI shows it in a dialog that says what failed and what the user can do: retry, log in again, or go back. The message is written to `install.log` too. A test checks that every `xc_err` value has a message and a suggested action.

   *Implemented 2026-10-27:* `xc_err_action` gives each code one sentence saying what to do, and `xc_err_retry` whether trying again can help; `test_bytes` checks every code has both, that they agree (an action that says "try again" goes with RETRY), and that each fits the dialog. The install end dialog shows the action after the error, and RETRY only where `xc_err_retry` says so, else BACK alone.
6. **A scripted input mode for the exit test's xemu walkthrough.** If `E:\xcat\ui-script.txt` exists, the app reads controller presses (and waits) from it instead of the pad, and logs each screen it reaches. Its exact format is specified in the PR that adds it, in `docs/testing.md`.
7. **One bundled font: Noto Sans** (SIL Open Font License 1.1), Regular and Bold. It is vendored under `third_party/fonts/` with its licence text, listed in THIRD_PARTY.md, and shipped next to the XBE. It covers the Latin, Greek and Cyrillic in the catalog's titles. A character it lacks is drawn as a box, never skipped.

   *Amended 2026-10-21 (owner decision, [ui-style.md](../ui-style.md#fonts)):* two display faces join it, for the look of the owner's mockups: **Michroma** for screen titles and **Chakra Petch Bold** for section names and labels, both SIL OFL 1.1, static TrueType, vendored and pinned the same way. They lack Greek and Cyrillic, so they draw only the app's own fixed words; game names and all other text stay in Noto Sans.

## Consequences

- No new library: SDL2, SDL_ttf and FreeType are already part of nxdk, and the app already links SDL. Only the font is new, at about 0.5 MB per face, which must ship with every release under its OFL licence.
- nxdk's SDL_ttf carries **FreeType 2.4.12** (2013), which has known bugs in parsing hostile fonts. So the app only ever loads the font it ships, from its own folder: no font comes from a map, a download or the user. FreeType's licence (FTL) asks for a credit in the documentation, so its notice joins the release's `CREDITS.txt`. Upgrading FreeType is nxdk's to do; if its pinned version changes, this note is rechecked.
- We write and maintain the widgets. There are about six of them, sized for one resolution, which is less than adapting a mouse-centred library to a controller.
- The screen logic is testable on the PC, so the exit test's error paths can be covered in CI and not only by hand.
- Software rendering at 640×480 limits how much can move at once: no animated backgrounds, and smooth scrolling only if it measures well on hardware. Box art (none in M7, ADR 0014) would add texture memory. pbkit stays possible later for a renderer behind the same drawing interface.
- The debug-console list and its 63-column workarounds go away.
