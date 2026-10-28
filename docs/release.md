# Making a release

How a version of the app gets to GitHub's Releases page ([ADR 0017](adr/0017-release-packaging.md)). CI builds the zip from a tag and leaves a **draft**; the owner attaches the catalog, checks it and publishes it.

## What a release has

| File | From |
|---|---|
| `xiso-cartographer-X.Y.Z.zip` | CI, from the tag (`.github/workflows/release.yml`, `tools/release/package.py`) |
| `xiso-cartographer-X.Y.Z.zip.sha256` | CI |
| `catalog.bin` | The owner, built from the locked catalog cache ([tools/catalog/README.md](../tools/catalog/README.md)) |
| `CREDITS.txt` | The owner, from the same catalog build: the catalog's sources and FreeType's credit |

The zip holds one folder, `xiso-cartographer\`, to copy to the console as it is:

- `default.xbe`, with its title image ([xbe-title-image.md](formats/xbe-title-image.md)), and the `fonts\` and `ui\` folders it needs;
- `README.txt`: what it is, how to install it, and what it needs;
- `LICENSE.txt`: the project's MIT licence;
- `THIRD_PARTY.txt` and `licenses\`: every third-party component in the XBE, its licence text, and FreeType's credit ([THIRD_PARTY.md](../THIRD_PARTY.md)).

The bundled test image (`samples\`) isn't in it. The same tag always gives the same zip, byte for byte: the files are sorted and dated by the tagged commit.

## Steps

1. **Version PR.** Set `XC_VERSION_MAJOR`, `_MINOR` and `_PATCH` in [`core/include/xcat/version.h`](../core/include/xcat/version.h). In `CHANGELOG.md`, rename `[Unreleased]` to `[X.Y.Z] - YYYY-MM-DD` and start a new empty `[Unreleased]` above it. Merge it once CI is green.
2. **Tag** that commit on `main`, and push the tag:

   ```bash
   git tag -a vX.Y.Z -m "xiso-cartographer X.Y.Z" && git push origin vX.Y.Z
   ```

   The Release workflow builds the XBE, packs the zip, and creates the draft release `xiso-cartographer X.Y.Z`. A tag that doesn't match `version.h` fails the workflow, and nothing is created.
3. **Catalog.** Build `catalog.bin` and `CREDITS.txt` from the cache that matches `tools/catalog/inputs.lock` (`build.py` without `--update-lock` checks it), and attach them to the draft:

   ```bash
   gh release upload vX.Y.Z build/catalog/catalog.bin build/catalog/CREDITS.txt
   ```

4. **Check** the draft: the zip's SHA-256 matches its `.sha256`, and the zip unpacks to `xiso-cartographer\` with the XBE, `fonts\`, `ui\`, `README.txt`, `LICENSE.txt`, `THIRD_PARTY.txt` and `licenses\`. Optionally copy it to a console and start it: the startup screen shows the new version.
5. **Publish** the draft on GitHub.

## Every PR

The CI `xbe` job packs a zip too, without a tag, as part of its artifact (`dist/`). So a component added without its licence, or a packer that breaks, fails a PR long before a release.

Adding a library to the XBE? Give it a row in [THIRD_PARTY.md](../THIRD_PARTY.md) and add its licence file to `LICENSES` in [`tools/release/package.py`](../tools/release/package.py).
