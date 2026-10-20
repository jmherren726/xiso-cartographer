# 0013. The archive.org source: entries keyed by file name, checked against the item's listing

- **Status:** Accepted (owner decision, 2026-10-20)
- **Date:** 2026-10-20
- **Amends:** [ADR 0010](0010-on-box-login-required-for-beta.md) decision 6 (`tools/xcat-login` isn't built in M6)

## Context

M6 makes `@archiveorg <identifier>` in a map ([map.md §5.2](../formats/map.md#52-archiveorg-identifier)) generate entries from an archive.org item, using the public metadata API (`https://archive.org/metadata/<identifier>`). The owner's items are collections. One holds about 140 full-disc Deflate zips of 5–8 GB each, plus a few metadata files. Its listing is about 43 KB of JSON. For each file it gives the name, the size (as a decimal string), the MD5, SHA-1 and CRC-32 of the whole file, `source` (`original`, `derivative`, `metadata`) and `private`. The files themselves need the user's login, but anyone can read the listing. For an item that doesn't exist, the API answers `200` with `{}`, and a taken-down item has `"is_dark": true`.

A map entry's key is the title ID from the game's XBE certificate ([map.md §4.1](../formats/map.md#41-key)). The listing has no title IDs, only file names. The title ID could be read from each image's `default.xbe` with a few `Range` requests, but only in an image that can be read at random. A Deflate zip, the owner's whole collection, can only be read from the front, and `default.xbe` can be gigabytes in. The catalog that will match names to title IDs comes in M7.

The PC login helper `tools/xcat-login` was optional in M6 (ADR 0010 decision 6). The owner pastes their archive.org cookie into the credentials file, and M8 brings the on-box login. So the helper would be throwaway work.

## Decision

1. **Generated entries are keyed by name.** Each image file in the listing becomes an entry keyed by its file name without the extension (`Game (USA)` for `Game (USA).zip`) and has no title ID. Entries written by hand keep their title-ID keys, and the map's file syntax doesn't change. The install reads the title ID from the installed `default.xbe` and records it. From M7, the catalog matches names to title IDs.
2. **Which files.** Files whose name ends in `.iso`, `.xiso` or `.zip` (any case) and whose `source` is `original`. Derivatives and metadata files are skipped. Each file's URL is `https://archive.org/download/<identifier>/<name>`, percent-encoded. The rules are in [archiveorg.md](../formats/archiveorg.md).
3. **Checked against the listing.** Each entry carries the file's size and whichever of MD5, SHA-1 and CRC-32 the listing gives. An install refuses to start if the server's file isn't that size. For trimmed xisos, which are hashed whole ([journal.md §7](../formats/journal.md#7-verification)), the MD5 and SHA-1 must match. A zip's hashes cover the zip, not the image. The member's own CRC-32 already covers every byte of the image (ADR 0009), so the zip isn't hashed again. A full-disc `.iso` skips its video partition, so it can't be hashed whole either; the size check and the read-back CRC-32s apply.
4. **Missing and removed items are errors that name the item.** An empty answer is "archive.org item `<id>` doesn't exist or was removed". `is_dark` is "archive.org item `<id>` was taken down". Either way the directive's line fails (a line error, [map.md §7](../formats/map.md#7-errors-and-warnings)), every other entry still loads, and nothing tries a mirror or another item.
5. **The listing is untrusted.** It is read with a strict, bounded JSON parser (4 MiB at most, nesting 32 deep), checked field by field, and fuzzed. A file with a bad name or size is skipped with a warning, and one bad hash only drops that hash.
6. **No PC login helper in M6.** Logins stay a token in the credentials file, written by hand until M8's on-box login.

## Consequences

- `@archiveorg` works for every image type, including the owner's Deflate zips, without downloading anything but the listing.
- Until M7, a generated entry can be found by name, not by title ID. The console's install list (which shows names anyway) works now. The catalog UI waits for M7's name matching.
- Map loading needs the network for `@archiveorg`, so the console starts the network before it loads the map.
- Each map load fetches the listing again (about 43 KB per item). It isn't cached; one can be added if loads get slow.
- The size check catches a listing that disagrees with the file before any download, and the hashes the project already computes are compared, so nothing new has to be hashed.
