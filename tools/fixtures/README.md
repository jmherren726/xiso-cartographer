# Synthetic xiso fixtures

Test images for the `xdvdfs` reader and the extractor, built from **generated dummy data only**. No game content ever goes into a fixture, the repo or CI ([ADR 0006](../../docs/adr/0006-no-links-no-bypass.md)).

```sh
make fixtures                          # → build/fixtures/<name>.iso + <name>.json
make fixtures FIXTURES_ARGS=--large    # also the >4 GiB image
make fixtures-verify                   # generate, then cross-check with extract-xiso (Docker)
python3 tools/fixtures/fixtures.py --out <dir> [--large] [--only NAME ...]   # without make
```

Output is git-ignored, deterministic (byte-identical on every run and platform) and **sparse**. Zero regions are never written, so the full-disc images (7–9 GB logical) use well under 1 MB of disk. That holds on ext4, APFS and NTFS (the generator sets the NTFS sparse flag). Filesystems without sparse files (FAT32, exFAT) can't hold the full-disc or large images.

## Fixtures

| Name | Layout | Image size | What it exercises |
|---|---|---|---|
| `small` | xiso | 768 KiB | Files of 0 B, 1 B, exactly one sector, one sector + 1, larger; two subdirectories, one nested; a name with a space |
| `deep` | xiso | 1.6 MiB | 12 levels of nesting; a 300-entry directory whose table spans several sectors; names of 42, 43 and 255 characters; mixed case; an empty directory and a directory containing only an empty directory |
| `redump-xgd1` | Full disc, partition at `0x18300000` | 7.8 GB (sparse) | Partition detection and partition-relative sectors (XGD1); same tree as `small` |
| `redump-xgd2` | Full disc, partition at `0x0FD90000` | 7.8 GB (sparse) | The same for XGD2 |
| `redump-xgd3` | Full disc, partition at `0x02080000` | 8.7 GB (sparse) | The same for XGD3 |
| `large` (`--large`) | xiso | 4.3 GB (sparse) | A file of 4,294,965,248 bytes (near the 32-bit size limit), then files whose offsets are past 4 GiB; 64-bit offset arithmetic |

Negative cases (corrupt or hostile images) belong to the M1 parser tests and fuzzing, not this generator. The generator refuses to write invalid images.

## Manifest (`<name>.json`, `<name>.tsv`)

```json
{
  "format": "xcat-fixture-manifest", "version": 1,
  "name": "small", "layout": "xiso", "partition_offset": 0,
  "image_size": 786432, "image_sha256": "…",
  "root": { "sector": 264, "size": 2048 },
  "files": [ { "path": "data/level01.dat", "type": "file", "sector": 268, "offset": 548864,
               "size": 65536, "sha256": "…" }, … ],
  "dirs":  [ { "path": "data", "type": "dir", "sector": 265, "offset": 542720, "size": 2048 }, … ]
}
```

- `files` is sorted by `offset`, which is the order the `xdvdfs` reader must produce. `offset` is absolute in the image: `partition_offset + sector × 2048`. Paths use `/`.
- `size` of a directory is its table size in bytes (a whole number of sectors).
- `image_sha256` is `null` for images over 64 MiB, so nobody has to hash GBs of holes.
- A new major `version` means existing tests must be updated.
- `<name>.tsv` carries the same data for C tests, one record per line: `layout <layout> <partition_offset>`, `root <sector> <size>`, then `file|dir <offset> <size> <sector> <path>` (files offset-sorted, dirs by path).
- CMake generates the default set into `build/<preset>/fixtures/` before the tests run (`xcat_fixtures` target); `-DXCAT_LARGE_FIXTURES=ON` adds `large`.

## File content

Each file's content is defined so any language can reproduce any byte range:

- **`pattern`:** byte *i* of the file is byte *i mod 64* of BLAKE2b-512(`"<seed>:<i div 64>"`), where `seed` is `<fixture>/<path>`.
- **`sparse`:** zeros, except the first and last 4096 bytes, which follow `pattern`. Used for the 4 GB file in `large`.

## Image layout

The structures, the name ordering (bytewise, `a`–`z` folded to upper case) and the padding rules are those extract-xiso reads and writes; they're summarised at the top of [xisogen.py](xisogen.py). Choices the format leaves open:

- the root table sits at sector `0x108`, then the other directory tables, then file data in path order;
- the tree in each table is balanced, with the root entry at offset 0;
- xiso images are padded to a multiple of 64 KiB; full-disc layouts to a full dump's size.

A reader must accept any valid placement, not just this one. Real images differ.

## Fuzz seeds

`fuzz_seeds.py` turns xiso fixtures into seeds for `core/tests/fuzz_xdvdfs.c`. A seed is a small header followed by the image's directory tables, without file data. CMake runs it at build time into `build/<preset>/fuzz-seeds/`.

## Verification

- `test_xisogen.py`: unit tests for the generator's rules (run by CI with `python3 -m unittest discover -s tools/fixtures`).
- `verify.sh`: extracts every image with **extract-xiso** (an independent implementation, shipped in the nxdk image) and checks the result against the manifest with `check_extracted.py`: every file's size and SHA-256, every directory, and no extra files. CI runs it on the full set, including `--large`, and also checks that two runs produce identical output.
