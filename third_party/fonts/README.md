# Fonts

The UI's fonts ([ADR 0015](../../docs/adr/0015-ui-sdl2-own-widgets.md) decision 7, [ui-style.md](../../docs/ui-style.md#fonts)), vendored unmodified, each under the SIL Open Font License 1.1 with its own licence file:

- **Noto Sans**, Regular and Bold ([OFL.txt](OFL.txt)): all text, and the only one for game names and anything else from outside the app.
- **Michroma** ([OFL-Michroma.txt](OFL-Michroma.txt)): screen titles.
- **Chakra Petch** Bold ([OFL-ChakraPetch.txt](OFL-ChakraPetch.txt)): section names and labels.

Michroma and Chakra Petch cover Latin with its accents and "…", but not Greek or Cyrillic, so they draw only the app's own fixed words. They ship next to the XBE (`fonts\` in the XBE's folder), and the app loads no other font: nxdk's SDL_ttf carries an old FreeType that must never see a font from anywhere else (ADR 0015, Consequences). All are static TrueType fonts: that FreeType predates variable fonts.

| File | Version | From |
|---|---|---|
| `NotoSans-Regular.ttf` | Noto Sans v2.015, hinted TTF | [notofonts/notofonts.github.io@28b15b4](https://github.com/notofonts/notofonts.github.io/tree/28b15b4b43b7bed62b5cf6e6b0b5ff5846270535/fonts/NotoSans/hinted/ttf) (2024-11-21) |
| `NotoSans-Bold.ttf` | the same | the same |
| `OFL.txt` | — | [notofonts/latin-greek-cyrillic, tag NotoSans-v2.015](https://github.com/notofonts/latin-greek-cyrillic/blob/NotoSans-v2.015/OFL.txt) |
| `Michroma-Regular.ttf` | Michroma 1.100 (Vernon Adams), hinted TTF | [google/fonts@45e3d52 `ofl/michroma/`](https://github.com/google/fonts/tree/45e3d5269e4a17d823f6d84470bc084dc5ae46f6/ofl/michroma) (2026-10-21; upstream [googlefonts/Michroma-font](https://github.com/googlefonts/Michroma-font)) |
| `OFL-Michroma.txt` | — | the same folder's `OFL.txt` |
| `ChakraPetch-Bold.ttf` | Chakra Petch 1.000 (Cadson Demak), hinted TTF | [google/fonts@45e3d52 `ofl/chakrapetch/`](https://github.com/google/fonts/tree/45e3d5269e4a17d823f6d84470bc084dc5ae46f6/ofl/chakrapetch) (upstream [m4rc1e/Chakra-Petch](https://github.com/m4rc1e/Chakra-Petch)) |
| `OFL-ChakraPetch.txt` | — | the same folder's `OFL.txt` |

`SHA256SUMS` pins every file; the XBE build checks them before copying the fonts (`app/ui/ui-assets.mk`). Noto Sans covers Latin, Greek and Cyrillic. The OFL lets the fonts be bundled and redistributed with the app as long as their licence files go with them and they aren't sold on their own.

## Updating (a dedicated PR)

1. Take `NotoSans-Regular.ttf` and `NotoSans-Bold.ttf` from `fonts/NotoSans/hinted/ttf/` of a newer `notofonts.github.io` commit, and `OFL.txt` from the matching `NotoSans-v*` tag of `notofonts/latin-greek-cyrillic`; Michroma and Chakra Petch, with their `OFL.txt`, from a newer `google/fonts` commit. Never a variable font (`[wght]` in the name).
2. Check that `git hash-object <file>` equals the blob SHA GitHub shows for each file, then rewrite `SHA256SUMS` (`sha256sum` of every file but `README.md` and `SHA256SUMS`, two spaces between hash and name).
3. Update the table above and `THIRD_PARTY.md`, and run the `ui-test` XBE (docs/testing.md) to see the text.
