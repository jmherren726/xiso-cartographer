# Catalog pipeline

Builds `catalog.bin` ([catalog.md](../../docs/formats/catalog.md)) and `CREDITS.txt` from Wikidata and Redump ([ADR 0014](../../docs/adr/0014-catalog-sources-and-format.md), [SOURCES.md](SOURCES.md)).

**Status (M7):** design. The format and the rules below come first; the pipeline itself lands in the following PRs.

## Steps

1. **Fetch** (deliberate, occasional, never in CI): Wikidata through SPARQL, Redump's Xbox discs one request at a time, at least 1 s apart. Every raw response goes to a cache outside the repo, and its SHA-256 goes into `inputs.lock`.
2. **Build** (offline, deterministic): read the cache, refuse inputs whose hashes don't match `inputs.lock`, join and normalise, apply `overrides.toml`, write `catalog.bin` and `CREDITS.txt`, and print the catalog's SHA-256 (also recorded in `inputs.lock`).
3. **Check:** the same schema checks the console's reader makes, plus a report of releases matched by name only and games without metadata, for review.

## Title IDs from serials

A Redump serial `XX-NNN` (two upper-case letters, a hyphen, a decimal number up to 65,535) gives the title ID `XX` as two bytes followed by the number as 16 bits: `MS-004` → `4D530004`, `EA-013` → `4541000D`. A release with several serials uses the first one that fits. One with none that fits needs a title ID in `overrides.toml`; until it has one, it is left out and listed in the report.

## Variant names

Map keys pick a release with `<title-id>@<variant>` ([map.md §4.1](../../docs/formats/map.md#41-key)). Variants are derived the same way on every build:

1. Each Redump region becomes a code: `USA` → `US`, `Europe` → `EU`, `Japan` → `JP`, `Asia` → `AS`, `Korea` → `KR`, `Australia` → `AU`, `World` → `WW`, and any other country its ISO 3166-1 alpha-2 code (`Germany` → `DE`, `France` → `FR`). The codes are joined with `-` in Redump's order: `USA, Europe` → `US-EU`. A region with no code is an error in the build report, and is fixed in `overrides.toml`.
2. If two releases of the same title ID now share a variant, each of them gets `-` and its version or edition appended, as upper-case letters and digits only (`Platinum Hits` → `US-PLATINUMHITS`, `v1.01` → `US-V101`).
3. If they still collide, they get `-2`, `-3`, … in order of Redump disc number, and the first keeps its name.
4. A name longer than 16 characters is cut to 16 before any `-N` suffix of step 3 is added, then the suffix replaces its last characters.

A variant that `overrides.toml` sets wins over all of this. Changing a published variant breaks users' map keys, so the build report lists every variant that differs from the previous `catalog.bin`.
