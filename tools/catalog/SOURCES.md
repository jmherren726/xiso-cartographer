# Catalog sources

Every source the catalog pipeline reads, what it takes, and on what terms ([ADR 0014](../../docs/adr/0014-catalog-sources-and-format.md), [HANDOFF §6.3](../../docs/HANDOFF.md#63-legal-hygiene)). Update this file whenever a source, the fields taken, or its terms change. `CREDITS.txt` in each release is generated from it.

| Source | Taken | Not taken | Licence / terms | Access | Checked |
|---|---|---|---|---|---|
| [Wikidata](https://www.wikidata.org/) | For items that are video games (P31 Q7889) for the Xbox (P400 Q132020): item number, English label, developers (P178), publishers (P123), first publication date (P577, year only), genres (P136), Redump IDs (P5290) | Descriptions, images, sitelinks | [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/) for structured data; no conditions. Credited as a courtesy | Public SPARQL endpoint (`query.wikidata.org`), a few queries per refresh | 2026-10-20 |
| [Redump](http://redump.org/) | For Microsoft Xbox discs: disc number, title, region, serial, version, edition, full-disc image size, CRC-32, MD5, SHA-1 | Comments, barcodes, ring codes, security sectors, dumper names, scans, cue sheets, the datfile as a file | **No licence stated**; pages say "© Redump Team". Only factual fields are taken and the database isn't redistributed (ADR 0014, Consequences). Credited | The Xbox datfile (names, hashes), list pages (disc numbers) and disc pages (serial, region, track SHA-1), one request at a time, at least 1 s apart, cached; disc pages are fetched once ([README](README.md#fetching)) | 2026-10-20 |

## Rejected

| Source | Why |
|---|---|
| Wikipedia ("List of Xbox games") | CC BY-SA 4.0 would make the catalog share-alike; Wikidata has the same facts under CC0 |
| TheGamesDB, libretro-thumbnails (box art) | Art belongs to the publishers; no art in M7 (ADR 0014 decision 4) |
| IGDB, MobyGames | Terms forbid redistribution |

## Our own data

`overrides.toml` (joins, title-ID corrections, display titles, exclusions) is written by this project and released under [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/), like the catalog's own content.
