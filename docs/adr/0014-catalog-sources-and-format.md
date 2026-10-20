# 0014. The catalog: Wikidata and Redump data, no art, a checked binary file built by a deterministic pipeline

- **Status:** Accepted (owner decision, 2026-10-20)
- **Date:** 2026-10-20
- **Resolves:** HANDOFF §8 item 5 ("Catalog sources and licences, before M7")

## Context

M7 builds the catalog the console browses ([HANDOFF §4.1](../HANDOFF.md#41-roadmap); exit test: deterministic rebuild, schema tests, licences reviewed and credited). The catalog has to do three jobs:

1. **Show games:** a title, plus the developer, publisher, year and genre to browse and search by.
2. **Name map keys:** a map entry is keyed by title ID and variant ([map.md §4.1](../formats/map.md#41-key)), and the catalog is where the valid variants come from.
3. **Match names:** `@archiveorg` entries are keyed by file name, which is the Redump name in the collections seen so far ([ADR 0013](0013-archiveorg-source.md)). The catalog has to turn `Halo - Combat Evolved (USA)` into a title ID.

The sources considered (checked 2026-10-20):

| Source | Has | Licence | Notes |
|---|---|---|---|
| **Wikidata** | 1,036 items that are video games for the Xbox (P31 Q7889, P400 Q132020): title, developer (886), publisher (886), date (965), genre (968). 505 of them link 1,102 Redump disc IDs (P5290) | **CC0**: no conditions | No title-ID property. Queried through the public SPARQL endpoint |
| **Redump** | One page per pressed disc: title, region, languages, **serial** (e.g. `EA-013`), version, edition, and the full-disc image's size, CRC-32, MD5 and SHA-1. Also a datfile | **None stated**; pages say "© Redump Team" | The de facto naming standard: archive.org collections use its names |
| Wikipedia | "List of Xbox games": titles, developers, publishers, dates | CC BY-SA 4.0 | The catalog would become share-alike data. Wikidata has the same facts under CC0 |
| TheGamesDB, libretro-thumbnails | Box art | Art belongs to the publishers; uploads by users | An API key and terms for TheGamesDB; rights unclear for both |
| IGDB, MobyGames | Rich metadata | Terms forbid redistribution | Not usable |

An Xbox title ID ([xbe.md §2](../formats/xbe.md#2-certificate)) is, by convention, the publisher's two letters followed by a 16-bit number, and a disc's serial is usually the same thing written differently: `MS-004` is title ID `4D530004` (`M` = 0x4D, `S` = 0x53, 4 = 0x0004), and `EA-013` is `4541000D`. So Redump's serials give title IDs.

## Decision

1. **Sources: Wikidata and Redump, nothing else.**
   - Games (title, developer, publisher, year, genres) come from **Wikidata**, which is CC0.
   - Releases (title ID, variant, region, Redump name and disc number, image size and hashes) come from **Redump**. Only factual fields are taken: names, regions, serials, versions, sizes, hashes. No descriptions, comments, scans or dumper names.
   - A release's title ID is derived from its serial as above. A serial that doesn't fit the pattern, or a release whose installed XBE says otherwise (installs record the title ID, ADR 0013), is fixed in the overrides file (decision 3).
2. **Joining them:** a release belongs to the Wikidata game that links its Redump disc ID (P5290). For the other releases, the pipeline matches the Redump title to Wikidata titles after normalising punctuation and case, and **only** if exactly one game matches. *(2026-10-20, with the pipeline: then the one game that other releases with the same title ID joined; the full order is in [tools/catalog/README.md](../../tools/catalog/README.md#games).)* A release that still matches nothing becomes a game of its own, with the title from Redump and no other metadata. It is still installable and searchable.
3. **Human fixes live in one committed file**, `tools/catalog/overrides.toml`: forced joins, title-ID corrections, display titles, and excluded entries (non-games, demos if wanted). It is our own work, reviewed in PRs. The pipeline never edits it.
4. **No art in M7.** Box art belongs to the publishers and no source licenses it cleanly. The catalog is text only. The format has room for art later (a minor version can add a field). Art supplied by the user, or a source with a clean licence, needs its own ADR.
5. **The pipeline lives in this repo, the data doesn't.** `tools/catalog/` (Python) holds the scraper, the normaliser, the writer, `overrides.toml`, `SOURCES.md` and the tests.
   - The raw responses are cached on the machine that builds and **never committed**. Redump's data isn't ours to republish as a database, and the cache is easy to rebuild.
   - `tools/catalog/inputs.lock` records the SHA-256 of every cached raw input, and the SHA-256 of the `catalog.bin` they produce.
   - `catalog.bin` and `CREDITS.txt` are built from a cache whose hashes match the lock, and attached to GitHub releases next to the XBE ([HANDOFF §6.2](../HANDOFF.md#62-versioning-and-releases)). They aren't committed either.
6. **Deterministic.** The same raw inputs and overrides give a byte-for-byte identical `catalog.bin` on every machine. The build has no clock, no locale, no hash-order or network dependence. Every sort uses one documented collation, and the snapshot date is taken from the inputs. CI builds a small synthetic input set twice and compares the outputs, and checks the schema (catalog.md) on every PR.
7. **A good guest** ([ADR 0006](0006-no-links-no-bypass.md)): one request at a time with at least 1 s between requests to Redump, a User-Agent that names the project, and every response cached. A refresh is an occasional, deliberate run by a maintainer, never part of a normal build or of CI.
8. **Format: `catalog.bin`**, a small binary file specified in [catalog.md](../formats/catalog.md): a CRC-checked header, fixed-size game and release records, a name index and a string table. It is chosen over TSV so the console reads it without parsing text, by binary search, and checks it in one pass. It holds no URLs: Redump and Wikidata appear as numbers, never as links. Data still counts as untrusted on the console, so the reader is fuzzed like every other parser.
9. **Credits and licence of the output.** `CREDITS.txt` credits Wikidata (CC0, credited as a courtesy) and the Redump project, with the snapshot date. The catalog's own content (the joins and overrides) is released under CC0, so the data carries no conditions beyond those of its sources. `tools/catalog/SOURCES.md` records each source, what is taken from it, its licence or the lack of one, and the date it was checked.

## Consequences

- The catalog's data has no share-alike or attribution conditions from Wikidata. Redump's lack of a stated licence is the open risk. Only facts are taken, the project credits Redump, and the database itself is never redistributed. If Redump asks, its fields come out: the catalog then keeps games and title IDs from the installs and overrides, but loses names for name matching.
- About half of the Xbox games link to Redump through Wikidata. Name matching and `overrides.toml` cover the rest, and reviewing that file is the main manual work of a refresh.
- Without art, the M8 UI shows text. That keeps the release small (the catalog is well under 1 MiB).
- Anyone can check a release's catalog: fetch the same inputs (if the sources haven't changed), compare their hashes with `inputs.lock`, rebuild, and compare the result's SHA-256. When the sources have moved on, the rebuild is a new snapshot, not a mismatch.
- The pipeline is Python on a PC. The console only reads the result, through `xcat/catalog.h`.
