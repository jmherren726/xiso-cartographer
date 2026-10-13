# 0002. License the project under MIT

- **Status:** Accepted
- **Date:** 2026-10-13

## Context

The repository was first created with GPL-3.0. The feasibility study ([PLAN.md](../PLAN.md) §4) and the [handoff](../HANDOFF.md) §5.1 both recommended a permissive licence (MIT/BSD), so the choice was reviewed before any code landed.

The project's value is mostly in `libxcat`, its portable core (xiso streaming extraction, HTTP Range client, map parser). We want other dashboards and tools to be able to reuse it, including ones that aren't GPL.

Licences of the planned dependencies:

| Dependency | Licence | Used as |
|---|---|---|
| nxdk (much of it derived from OpenXDK) | MIT | Toolchain and runtime |
| pbkit (in nxdk) | MIT | Graphics |
| lwIP (in nxdk) | Modified BSD | Network stack |
| PDCLib (in nxdk) | CC0 | C library |
| LLVM compiler-rt parts (in nxdk) | MIT | Runtime |
| SDL2 / SDL_ttf (nxdk ports) | zlib | UI, if chosen (ADR before M8) |
| BearSSL | MIT | TLS |
| miniz / cJSON (if used) | MIT | Zip / JSON |

Sources: the nxdk README and the `LICENSES` folder at the pinned commit, plus each project's own licence file. nxdk's build-time tools (e.g. the NVIDIA Cg compiler) run on the host and are not part of the shipped XBE.

## Decision

xiso-cartographer is licensed under the **MIT License**. Before anything was released, the GPL-3.0 `LICENSE` was replaced in MR-1.

## Consequences

- Every dependency above is permissive and compatible with MIT. Release zips must include their notices: `THIRD_PARTY.md` and the licence files ship with each release (e.g. the lwIP BSD notice).
- Anyone may embed `libxcat` or ship modified builds, including closed-source ones. We accept that: our legal stance depends on the project itself shipping no links or copyrighted content, not on the licence of forks.
- **GPL code can't be copied in.** Code from GPL Xbox projects (e.g. XBMC-derived code) must not be added, even if it's convenient. Write our own instead, or use a permissively licensed equivalent.
- Any new dependency's licence is checked in the MR that adds it. Anything not on the permissive list (MIT, BSD, zlib, ISC, Apache-2.0, CC0/public domain) needs a new ADR.
- When nxdk is added as a submodule (MR-4), recheck its `LICENSES` folder at the pinned commit and update this table if anything differs.

## Recheck: nxdk added in MR-4

Checked against nxdk `14d5ee9` (2026-09-30). Its `LICENSES/` folder has Apache-2.0, CC0-1.0, MIT, NCSA and LicenseRef-NVIDIA-CG. Its Docker image label also lists BSD-3-Clause and GPL-2.0-or-later. Two items weren't in the table above:

| Component | Licence | Where it ends up | Effect on us |
|---|---|---|---|
| `tools/cxbe` (EXE → XBE converter) | GPL-2.0-or-later | Build machine only. It reads our linked `main.exe` and writes the XBE container; none of its code is linked into the XBE. | None. Running a GPL tool doesn't relicense its input or output, just as compiling with GCC doesn't. We don't redistribute cxbe; the Docker image is built locally and in CI, never published. |
| `tools/cg` (NVIDIA Cg compiler binary) | Proprietary (LicenseRef-NVIDIA-CG) | Build machine only, and only for shader sources, which we don't have yet. | None now. Revisit if the UI uses Cg shaders and we ever publish the build image. |
| libc++ (`lib/libcxx`) | Apache-2.0 WITH LLVM-exception / NCSA | Linked only into C++ builds (`NXDK_CXX`). The C app doesn't link it. | Compatible with MIT. Include its notice if a later ADR adopts C++. |

The decision stands. If we ever publish the nxdk build image, revisit it: that would mean redistributing cxbe (GPL) and cgc (proprietary).
