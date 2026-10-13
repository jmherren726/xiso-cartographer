# Third-party code

Every dependency is a pinned git submodule under `third_party/` (or vendored with its licence file intact). Licence compatibility is covered by [ADR 0002](docs/adr/0002-license.md). Upgrades go in a dedicated MR.

| Component | Version | Licence | Path | Used for | Ships in release |
|---|---|---|---|---|---|
| [Unity](https://github.com/ThrowTheSwitch/Unity) | v2.6.1 | MIT | `third_party/unity` | Host unit tests ([ADR 0003](docs/adr/0003-unit-test-framework.md)) | No |
| [nxdk](https://github.com/XboxDev/nxdk) | `14d5ee9` (2026-09-30) | Mixed; see below | `third_party/nxdk` | XBE toolchain and runtime libraries | Linked parts only |

## nxdk components

nxdk brings its own submodules. Licences per nxdk's README and `LICENSES/` folder at the pinned commit:

| Component | Licence | Linked into our XBE |
|---|---|---|
| nxdk core (OpenXDK-derived), pbkit, hal, winapi, xboxrt | MIT | Yes |
| compiler-rt-derived runtime | MIT | Yes |
| PDCLib (C library) | CC0 | Yes |
| lwIP (network stack) | Modified BSD | Yes (from M3) |
| SDL2, SDL_ttf, SDL2_image | zlib | Only if the UI uses SDL (ADR before M8) |
| libpng, libjpeg-turbo, zlib | libpng / IJG + BSD / zlib | Only if used |
| libc++ | Apache-2.0 WITH LLVM-exception / NCSA | Only for C++ builds |
| cxbe | GPL-2.0-or-later | No: build-time tool ([ADR 0002](docs/adr/0002-license.md#recheck-nxdk-added-in-mr-4)) |
| NVIDIA Cg compiler (`cgc`) | Proprietary | No: build-time tool, shaders only |
| extract-xiso | BSD | No: build-time tool (creates the test ISO) |

Release zips must include the licence notices for every component marked "Yes", starting with the first release (M9).
