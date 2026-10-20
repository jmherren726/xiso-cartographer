# Catalog pipeline

Builds `catalog.bin` ([catalog.md](../../docs/formats/catalog.md)) and `CREDITS.txt` from Wikidata and Redump ([ADR 0014](../../docs/adr/0014-catalog-sources-and-format.md), [SOURCES.md](SOURCES.md)). Python 3.11 or newer, standard library only.

| File | Does |
|---|---|
| [`fetch.py`](fetch.py) | Fetches the raw inputs into a cache outside the repo |
| [`sources.py`](sources.py) | Parses them: Redump's datfile, list pages and disc pages; Wikidata's SPARQL answers (and holds the queries) |
| [`build.py`](build.py) | Checks the cache against `inputs.lock`, joins, applies `overrides.toml`, writes the outputs |
| [`catalog_bin.py`](catalog_bin.py) | Writes (and, for reports and tests, reads) `catalog.bin` |
| [`overrides.toml`](overrides.toml) | Human corrections, reviewed in PRs |
| `inputs.lock` | SHA-256 of the inputs and of the catalog they build (written by `build.py --update-lock`, from the first snapshot on) |
| [`test_pipeline.py`](test_pipeline.py) | Tests on a synthetic cache (CTest `catalog_pipeline`) |

## Running it

A refresh is a deliberate, occasional run by a maintainer, never part of a normal build or of CI (ADR 0014 decision 7):

```bash
python3 -I tools/catalog/fetch.py --cache ~/xcat-catalog-cache
```

```bash
python3 -I tools/catalog/build.py --cache ~/xcat-catalog-cache --out build/catalog --update-lock --previous <last release's catalog.bin>
```

Then review `build/catalog/report.txt`, fix what needs fixing in `overrides.toml`, rebuild, and commit `overrides.toml` and `inputs.lock` together. Without `--update-lock`, `build.py` refuses a cache that doesn't match the lock, and a catalog that differs from the one the lock records. That is how anyone with the same inputs checks a release's catalog.

### Fetching

`fetch.py` asks each host one request at a time, at least 1 s apart, with backoff on errors and a User-Agent that names the project. Redump's robots.txt allows everything (checked 2026-10-20).

1. Wikidata: the six queries in `sources.WIKIDATA_QUERIES` (games and their names, developers, publishers, genres, dates, Redump IDs). A name is the English (`en`) label, else the `mul` label: Wikidata keeps names that are the same in every language only under `mul`, and Halo: Combat Evolved is one of them.
2. Redump's Xbox datfile: every disc's Redump name and full-disc size, CRC-32, MD5 and SHA-1. It has no serials.
3. Redump's Xbox list pages (27 in 2026), for the disc numbers.
4. One page per disc, for its serial and region, and its track's SHA-1, which ties it to its datfile entry. Pages already cached aren't fetched again (`--refresh-discs` does), so only the first fetch takes long: about 2,700 discs, 45 minutes.

The cache layout is in `fetch.py`'s header. `meta.json` records the fetch dates, which become the catalog's `snapshot`.

### Inputs lock

`inputs.lock` has one line per group of inputs (`meta`, `wikidata`, `redump-datfile`, `redump-list`, `redump-disc`): the number of files and the SHA-256 of their sorted `path NUL sha256` lines. A last line gives the SHA-256 of `catalog.bin`. A mismatch names the group that changed.

## Releases

A release is a datfile entry whose category is in `overrides.toml`'s `categories` (default `Games`; the datfile also has Demos, Coverdiscs, Preproduction, Applications, Bonus Discs and Add-Ons), that holds exactly one `.iso`, and that `exclude` doesn't name. Its disc page is the one whose track has the same SHA-1. Its region is the first tag of its Redump name.

### Title IDs from serials

A Redump serial `XX-NNN` (two upper-case letters, a hyphen, a decimal number up to 65,535) gives the title ID `XX` as two bytes followed by the number as 16 bits: `MS-004` → `4D530004`, `EA-013` → `4541000D`. A release with several serials uses the first one that fits. One with none that fits, or with no disc page, needs a title ID in `[title_id]`; until it has one, it is left out and listed in the report.

## Games

A release joins a game in this order:

1. `[game]` in `overrides.toml`: a Wikidata game, or `none`.
2. The Wikidata game that links its Redump disc number (P5290).
3. The one Wikidata game whose name matches its title. Titles are compared as letters and digits only, in lower case, with `&` read as "and", after Redump's title is turned around (`Elder Scrolls III, The - Morrowind` → `The Elder Scrolls III: Morrowind`). Two or more matches is no match.
4. The one Wikidata game that other releases with its title ID joined.
5. Otherwise, a game without Wikidata data, shared by the releases with its title ID: one game is often sold under a different title in each region (`Finding Nemo`, `A Procura de Nemo`). Its title is that of its first release in the order USA, USA and Europe, World, Europe, UK, Australia, then any other, turned around as above. `[game] = "none"` takes a release out of its title ID's game into one of its own, shared only by releases whose titles compare equal (two discs of one set, two bundles).

The report lists every join made by steps 3 and 4, and every step-5 game with several titles, for review. A game's developers, publishers and genres are their names, joined with `, ` in §6.1 order, dropping the last until they fit 255 bytes; its year is the earliest publication date. `[title]` replaces a display title.

## Variant names

Map keys pick a release with `<title-id>@<variant>` ([map.md §4.1](../../docs/formats/map.md#41-key)). Variants are derived the same way on every build:

1. Each Redump region becomes a code: `USA` → `US`, `Europe` → `EU`, `Japan` → `JP`, `Asia` → `AS`, `Korea` → `KR`, `Australia` → `AU`, `World` → `WW`, `UK` → `GB`, and any other country its ISO 3166-1 alpha-2 code (`Germany` → `DE`, `France` → `FR`; the table is in `build.py`). The codes are joined with `-` in Redump's order: `USA, Europe` → `US-EU`. A region with no code stops the build, and is fixed in `[region]`.
2. If two releases of the same title ID now share a variant, each one whose Redump name has tags after its regions and languages gets them appended, as upper-case letters and digits, joined with `-`: `Game (USA) (Rev 1)` → `US-REV1`, while `Game (USA)` stays `US`.
3. If some still collide, they get `-2`, `-3`, … in order of Redump disc number, and the first keeps its name: `Game (Europe) (En,Fr)` → `EU`, `Game (Europe) (En,Es)` → `EU-2`.
4. A name longer than 16 characters is cut to 16 before any `-N` suffix of step 3 is added, then the suffix replaces its last characters.

`[variant]` wins over all of this. Changing a published variant breaks users' map keys, so `--previous` lists in the report every variant that differs from an older `catalog.bin`.
