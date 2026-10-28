# 0017. Releases: a tagged zip with every licence, built by CI as a draft

- **Status:** Proposed (accepted when the PR that adds it is merged by the owner)
- **Date:** 2026-10-28
- **Implements:** [HANDOFF §6.2](../HANDOFF.md#62-versioning-and-releases) and the M9 release; [THIRD_PARTY.md](../../THIRD_PARTY.md): "Release zips must include the licence notices for every component marked Yes"

## Context

M9 is the beta, the first release people download. Until now the only builds were CI artifacts (`app/bin`, disc images for xemu) and the version was `0.0.0`. HANDOFF §6.2 asks for SemVer tags and "CI on a tag builds the release zip". The XBE links code under MIT, CC0, BSD, zlib, the FreeType License and Apache-2.0, and embeds the MPL-2.0 CA bundle. Most of those licences ask for their notice to go with the binary, and FreeType's for a credit in the documentation. An audit for this ADR found one linked component missing from THIRD_PARTY.md: nxdk's USB host library, `libusbohci` (Apache-2.0, Nuvoton), which every nxdk XBE links for the controllers.

The catalog (`catalog.bin`, `CREDITS.txt`) is built from a cache that is never committed (ADR 0014 decision 5), so CI can't make it.

## Decision

1. **The version lives in `core/include/xcat/version.h`, as now.** A release starts with a PR that sets it and dates the CHANGELOG's Unreleased section. The tag `vMAJOR.MINOR.PATCH` then goes on that commit on `main`. The packer refuses a tag that doesn't match the header, so the app always shows the version it was released as.
2. **CI builds the release and leaves it as a draft.** A push of a `v*.*.*` tag runs `.github/workflows/release.yml`. It builds the XBE as the `xbe` job does, packs the zip, and creates a **draft** GitHub release with the zip and its SHA-256. The owner then attaches `catalog.bin` and `CREDITS.txt` (built from the locked cache, [tools/catalog/README.md](../../tools/catalog/README.md)), checks the release on hardware if wanted, and publishes it. Nothing is published unattended.
3. **The zip holds one folder, `xiso-cartographer\`, to copy to the console as it is:**
   - `default.xbe` with `fonts\` and `ui\`;
   - `README.txt`, with how to install and what the app needs;
   - `LICENSE.txt` (MIT);
   - `THIRD_PARTY.txt`, listing every third-party component in the XBE, its licence and FreeType's credit;
   - `licenses\`, each component's licence text as its project ships it, and a notice for the CA bundle saying where its MPL-2.0 source is;
   - the fonts' OFL texts, already next to them in `fonts\`.

   Text files have CRLF line endings for Windows and the console. **The bundled synthetic test image (`samples\`) is left out**: it's for testing, and without it the app says there is nothing to install until a map or an image is added. The list of licences is in one place, `tools/release/package.py`, and must follow THIRD_PARTY.md's "Ships in release" column.
4. **The zip is reproducible.** Entries are sorted and dated by the tagged commit (`SOURCE_DATE_EPOCH`), so the same tag gives the same bytes and SHA-256. The CI `xbe` job packs a zip on every PR too (as an artifact), so a missing licence or a broken packer shows before a tag.

## Consequences

- Cutting a release is: a version PR, a tag, then checking, attaching the catalog to, and publishing the draft. It's written down in [docs/release.md](../release.md).
- Every new linked library needs its row in THIRD_PARTY.md and its entry in the packer's list; the packer refuses to build if a listed licence file is missing.
- `libusbohci` joins THIRD_PARTY.md. Apache-2.0 is compatible with distributing the XBE under MIT terms for our own code (ADR 0002); its text and Nuvoton's disclaimer ship in `licenses\`.
- A user who wants to try the app without a map no longer gets the test image from a release; developers still get it from the build (`app/bin/samples`) and the CI artifact.
