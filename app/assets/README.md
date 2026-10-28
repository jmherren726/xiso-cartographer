# App artwork

| File | What | Origin and licence |
|---|---|---|
| `logo.png` | The app's logo, 512 × 512: the master the smaller sizes are made from | Made by the project owner (jmherren726) and released with the project under its [MIT licence](../../LICENSE), 2026-10-28. Scaled from the owner's 1254 × 1254 original (Lanczos, Pillow). |
| `title-image.png` | The same logo at 128 × 128: the XBE's title image, which dashboards show for the app ([docs/formats/xbe-title-image.md](../../docs/formats/xbe-title-image.md)) | As above. |

The XBE build puts `title-image.png` into `default.xbe` as its `$$XTIMAGE` section (`app/Makefile`, `tools/ui/xbe_title_image.py`). It must stay 128 × 128, 8-bit RGB or RGBA, not interlaced.

Never add game art, screenshots, or other people's logos or marks here (CLAUDE.md, [ui-style.md](../../docs/ui-style.md#where-it-comes-from)).
