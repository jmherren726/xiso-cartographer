# Catalog format (`catalog.bin`), version 1.0

**Status:** Normative. Written in M7, before the reader (`xcat/catalog.h`) and the pipeline (`tools/catalog/`) that implement it. The reasons, and where the data comes from, are in [ADR 0014](../adr/0014-catalog-sources-and-format.md).

The catalog lists the original Xbox games the app knows about: one **game** per work (Halo: Combat Evolved), and one **release** per pressed disc of it (USA, Europe, a Platinum Hits re-release). A release has the title ID from its XBE certificate, a variant name for map keys ([map.md §4.1](map.md#41-key)), its Redump name, and the size and hashes of its full-disc image. The pipeline builds the catalog on a PC; the console reads it from `E:\Apps\xcat\catalog.bin`, next to the XBE.

The catalog ships with the app, but the console still treats it as untrusted input. A user can replace it, and an HDD can corrupt it, so every offset, count and string is checked before use (§7). It holds no links ([ADR 0006](../adr/0006-no-links-no-bypass.md)) and no art (ADR 0014).

The words **must**, **must not**, **should** and **may** are used as in RFC 2119.

## 1. Layout

All integers are unsigned and little-endian. Offsets are in bytes from the start of the file. The file is:

| Part | Where | Contents |
|---|---|---|
| Header | 0–63 | §2 |
| Games | `games_off` | `game_count` records of `game_size` bytes (§3) |
| Releases | `releases_off` | `release_count` records of `release_size` bytes (§4) |
| Name index | `names_off` | `release_count` × u32 release indices (§5) |
| Strings | `strings_off` | `strings_size` bytes of NUL-terminated UTF-8 (§6) |

The parts **must** appear in this order, each starting on a multiple of 4, with no gaps other than up to 3 zero bytes of padding before each part, and the strings **must** end exactly at the end of the file.

## 2. Header

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 8 | `magic` | `XCATCAT` followed by a 0 byte |
| 8 | 2 | `major` | 1 |
| 10 | 2 | `minor` | 0 |
| 12 | 4 | `file_size` | Size of the whole file |
| 16 | 4 | `game_count` | 1–65,535 |
| 20 | 4 | `release_count` | 1–65,535 |
| 24 | 2 | `game_size` | Size of one game record, ≥ 32 |
| 26 | 2 | `release_size` | Size of one release record, ≥ 72 |
| 28 | 4 | `games_off` | |
| 32 | 4 | `releases_off` | |
| 36 | 4 | `names_off` | |
| 40 | 4 | `strings_off` | |
| 44 | 4 | `strings_size` | ≥ 1 |
| 48 | 4 | `snapshot` | String (§6): when the source data was taken, e.g. `wikidata 2026-10-20; redump 2026-10-20` |
| 52 | 8 | reserved | 0 |
| 60 | 4 | `crc32` | CRC-32 ([`xcat/crc32.h`](../../core/include/xcat/crc32.h), zlib variant) of the whole file except these 4 bytes: bytes 0–59, then 64 to the end |

- `file_size` **must not** exceed **8 MiB** (`XC_CATALOG_MAX_SIZE`). About 1,000 games and 1,500 releases need well under 1 MiB.
- Versioning follows [map.md §3](map.md#3-header-and-versioning). A reader rejects another major version. Minor versions only add things: a newer minor **may** make records longer (a reader uses `game_size` and `release_size` to step through them and ignores bytes it doesn't know) or use reserved bytes and flags that this version requires to be 0. A reader **must** ignore reserved fields and flags it doesn't know if the minor version is newer than its own, and **must** reject non-zero ones otherwise.
- The catalog has no build time. Two builds from the same source data are byte for byte identical (ADR 0014), and `snapshot` says which data that was.

## 3. Games

A game is one work, whatever its regions and editions. Records are 32 bytes in version 1.0:

| Offset | Size | Field | Meaning |
|---|---|---|---|
| 0 | 4 | `title` | String: the display title, e.g. `Halo: Combat Evolved`. Never empty |
| 4 | 4 | `developer` | String: developers joined with `, `; empty if unknown |
| 8 | 4 | `publisher` | String: publishers joined with `, `; empty if unknown |
| 12 | 4 | `genres` | String: genres joined with `, `; empty if unknown |
| 16 | 4 | `wikidata` | The Wikidata item number (`Q` + this number); 0 if the game has no Wikidata item |
| 20 | 2 | `year` | Year of the first release, 1990–2100; 0 if unknown |
| 22 | 2 | `flags` | 0 in version 1.0 |
| 24 | 8 | reserved | 0 |

- Games are in **display order**: by `title` under the collation of §6.1, and by `wikidata` for equal titles. A reader shows them in file order and **must** reject a file where they aren't sorted.
- Every game **must** have at least one release (§4).

## 4. Releases

A release is one pressed disc as Redump lists it. Records are 72 bytes in version 1.0:

| Offset | Size | Field | Meaning |
|---|---|---|---|
| 0 | 4 | `game` | Index of its game (§3), < `game_count` |
| 4 | 4 | `title_id` | Title ID from the disc's XBE certificate ([xbe.md](xbe.md)); never 0 |
| 8 | 4 | `variant` | String: the variant name for map keys (§4.1) |
| 12 | 4 | `name` | String: the Redump name, e.g. `Halo - Combat Evolved (USA)`. Never empty |
| 16 | 4 | `region` | String: regions as Redump writes them, e.g. `USA, Europe`; empty if unknown |
| 20 | 4 | `redump_id` | Redump's disc number; 0 if none |
| 24 | 8 | `size` | Size in bytes of the full-disc image (a Redump `.iso`) |
| 32 | 4 | `crc32` | CRC-32 of the full-disc image |
| 36 | 16 | `md5` | MD5 of the full-disc image |
| 52 | 20 | `sha1` | SHA-1 of the full-disc image |

- Releases are sorted by `title_id`, then by `variant` under §6.1, so a reader can find a title by binary search. A reader **must** reject a file where they aren't sorted, or where two releases have the same `title_id` and `variant`.
- The hashes describe Redump's full-disc dump. An install never sees that image whole (a trimmed xiso or a zip member is a different file, and full-disc installs skip the video partition, [archiveorg.md §8](archiveorg.md)), so the app uses them to show what a release is, not to verify installs.

### 4.1 Variants

- A variant name is 1–16 characters from `A–Z 0–9 - _` (upper case), which is the map's `variant` grammar ([map.md §4.1](map.md#41-key)) after it is normalised to upper case.
- Variant names are unique within a title ID, not across the catalog: `4D530004@US` and `4D530004@EU` are two releases of one title ID. How the pipeline derives the names (from the regions, then the version and edition when regions collide) is in [`tools/catalog/README.md`](../../tools/catalog/README.md); the reader only checks the rules above.
- A map key `<title-id>@<variant>` names one release. A key with no variant names every release of the title ID ([map.md §4.1](map.md#41-key)). A key whose title ID or variant isn't in the catalog is "not in catalog".

## 5. Name index

`release_count` u32 indices of releases, sorted by the releases' `name` under §6.1, each release exactly once. It finds the release for a map entry keyed by name ([archiveorg.md](archiveorg.md): `Halo - Combat Evolved (USA)` from `Halo - Combat Evolved (USA).zip`) by binary search. A reader **must** reject an index that isn't a permutation of the releases or isn't sorted. Two releases **must not** have names that compare equal.

## 6. Strings

- A string field is an offset into the strings part. The string runs from there to the next 0 byte, which **must** be inside the strings part. Offset 0 **must** point to a 0 byte, the empty string, and is what every empty field uses.
- Strings are UTF-8 ([RFC 3629](https://www.rfc-editor.org/rfc/rfc3629): no overlong forms, no surrogates, nothing above U+10FFFF) with no characters below U+0020 and no U+007F. Each is at most **255 bytes** without its 0 byte.
- Strings **may** be shared: several fields may point to the same bytes, and the pipeline writes each distinct string once. Offsets **must** point to the start of a string (offset 0, or the byte after a 0 byte).

### 6.1 Collation

Strings are ordered by comparing bytes, with ASCII `A–Z` folded to `a–z` first. Nothing else is folded, so the order is the same on every machine. Equal-comparing names are what §5 forbids.

## 7. Reading

A reader loads the whole file (it is small) and checks, in this order, before it uses anything:

1. The file is at least 64 bytes and at most `XC_CATALOG_MAX_SIZE`, and `file_size` equals its size.
2. `magic`, and the version (§2).
3. `crc32`. A mismatch is "the catalog file is damaged".
4. The counts, record sizes, offsets and padding of §1–§2.
5. Every string field (§6), the game and release records (§3, §4) and their order, and the name index (§5).

Any failure rejects the whole file: there is no partial catalog. The app then runs without one, and every map entry shows as "not in catalog" ([map.md §4.1](map.md#41-key)). The reader allocates nothing beyond the file's own bytes, and keeps no pointers into it that it hasn't checked.

## 8. Changes

| Version | Change |
|---|---|
| 1.0 | First version (M7) |
