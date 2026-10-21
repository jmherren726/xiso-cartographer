# Third-party code

Every dependency is a pinned git submodule under `third_party/` (or vendored with its licence file intact). Licence compatibility is covered by [ADR 0002](docs/adr/0002-license.md). Upgrades go in a dedicated MR.

| Component | Version | Licence | Path | Used for | Ships in release |
|---|---|---|---|---|---|
| [Unity](https://github.com/ThrowTheSwitch/Unity) | v2.7.0 | MIT | `third_party/unity` | Host unit tests ([ADR 0003](docs/adr/0003-unit-test-framework.md)) | No |
| [BearSSL](https://www.bearssl.org/) | `7bea48e` (master, 2026-04-06; v0.6 plus fixes) | MIT | `third_party/bearssl` | TLS 1.2, X.509, hashes ([ADR 0005](docs/adr/0005-bearssl-for-tls.md), [ADR 0012](docs/adr/0012-tls-trust-time-and-entropy.md)). Fetched from bearssl.org, so Dependabot can't track it: check upstream and CVE feeds at each release | Yes (from M5) |
| [CA bundle](https://curl.se/docs/caextract.html) (`cacert.pem`, Mozilla's root store as curl publishes it) | Mozilla data as of 2026-09-25, SHA-256 `a41b5d35…0505` (pinned in `cacert.pem.sha256`) | MPL-2.0 | `third_party/cacert` (vendored, unmodified) | Built-in TLS trust anchors ([ADR 0012](docs/adr/0012-tls-trust-time-and-entropy.md) §3); updated in a dedicated PR, see its README | Yes (from M5) |
| [miniz](https://github.com/richgel999/miniz) | 3.1.2 (`77d0dce`, 2026-07-01) | MIT | `third_party/miniz` | Its inflater (tinfl) only, for Deflate zip members ([ADR 0009](docs/adr/0009-install-from-zip-archives.md), [zip.md §9](docs/formats/zip.md)); built by `cmake/miniz.cmake` with its own `miniz_export.h` | Yes (from M5b) |
| [Noto Sans](https://github.com/notofonts/latin-greek-cyrillic) (Regular, Bold) | v2.015, hinted TTF (notofonts.github.io `28b15b4`, 2024-11-21), pinned in `SHA256SUMS` | SIL OFL 1.1 | `third_party/fonts` (vendored, unmodified, with `OFL.txt`) | The UI font ([ADR 0015](docs/adr/0015-ui-sdl2-own-widgets.md) decision 7); copied next to each XBE with a UI, with `OFL.txt` ([README](third_party/fonts/README.md)) | Yes (from M8) |
| [nxdk](https://github.com/XboxDev/nxdk) | `14d5ee9` (2026-09-30) | Mixed; see below | `third_party/nxdk` | XBE toolchain and runtime libraries | Linked parts only |

## nxdk components

nxdk brings its own submodules. Licences per nxdk's README and `LICENSES/` folder at the pinned commit:

| Component | Licence | Linked into our XBE |
|---|---|---|
| nxdk core (OpenXDK-derived), pbkit, hal, winapi, xboxrt | MIT | Yes |
| compiler-rt-derived runtime | MIT | Yes |
| PDCLib (C library) | CC0 | Yes |
| lwIP (network stack) | Modified BSD | Yes (from M3) |
| SDL2 | zlib | Yes: the controller (from M4) and the UI (from M8, [ADR 0015](docs/adr/0015-ui-sdl2-own-widgets.md)) |
| SDL_ttf | zlib | Yes, from M8 (ADR 0015) |
| FreeType 2.4.12 (inside SDL_ttf) | FreeType License (FTL), credit required in the documentation | Yes, from M8; loads only the bundled font (ADR 0015) |
| SDL2_image | zlib | Only if used |
| libpng, libjpeg-turbo, zlib | libpng / IJG + BSD / zlib | Only if used |
| libc++ | Apache-2.0 WITH LLVM-exception / NCSA | Only for C++ builds |
| cxbe | GPL-2.0-or-later | No: build-time tool ([ADR 0002](docs/adr/0002-license.md#recheck-nxdk-added-in-mr-4)) |
| NVIDIA Cg compiler (`cgc`) | Proprietary | No: build-time tool, shaders only |
| extract-xiso | BSD | No: build-time tool (creates the test ISO) |

Release zips must include the licence notices for every component marked "Yes", starting with the first release (M9).
