# Fonts

The UI's one font, Noto Sans ([ADR 0015](../../docs/adr/0015-ui-sdl2-own-widgets.md) decision 7), vendored unmodified under the [SIL Open Font License 1.1](OFL.txt). It ships next to the XBE (`fonts\` in the XBE's folder), and the app loads no other font: nxdk's SDL_ttf carries an old FreeType that must never see a font from anywhere else (ADR 0015, Consequences).

| File | Version | From |
|---|---|---|
| `NotoSans-Regular.ttf` | Noto Sans v2.015, hinted TTF | [notofonts/notofonts.github.io@28b15b4](https://github.com/notofonts/notofonts.github.io/tree/28b15b4b43b7bed62b5cf6e6b0b5ff5846270535/fonts/NotoSans/hinted/ttf) (2024-11-21) |
| `NotoSans-Bold.ttf` | the same | the same |
| `OFL.txt` | — | [notofonts/latin-greek-cyrillic, tag NotoSans-v2.015](https://github.com/notofonts/latin-greek-cyrillic/blob/NotoSans-v2.015/OFL.txt) |

`SHA256SUMS` pins the three files; the XBE build checks them before copying the fonts (`app/ui/ui.mk`). They cover Latin, Greek and Cyrillic. The OFL lets them be bundled and redistributed with the app as long as `OFL.txt` goes with them and they aren't sold on their own.

## Updating (a dedicated PR)

1. Take `NotoSans-Regular.ttf` and `NotoSans-Bold.ttf` from `fonts/NotoSans/hinted/ttf/` of a newer `notofonts.github.io` commit, and `OFL.txt` from the matching `NotoSans-v*` tag of `notofonts/latin-greek-cyrillic`.
2. Check that `git hash-object <file>` equals the blob SHA GitHub shows for each file, then rewrite `SHA256SUMS` (`sha256sum NotoSans-Regular.ttf NotoSans-Bold.ttf OFL.txt`).
3. Update the table above and `THIRD_PARTY.md`, and run the `ui-test` XBE (docs/testing.md) to see the text.
