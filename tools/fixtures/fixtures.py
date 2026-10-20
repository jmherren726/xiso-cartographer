"""Build the synthetic xiso fixture set used by the core tests.

    python tools/fixtures/fixtures.py --out build/fixtures            # default set
    python tools/fixtures/fixtures.py --out build/fixtures --large    # + >4 GiB image
    python tools/fixtures/fixtures.py --out build/fixtures --only small deep

For each fixture <name> this writes <name>.iso and <name>.json (the manifest:
every file's path, sector, absolute offset, size and SHA-256, offset-sorted,
plus the directories). Output is byte-for-byte deterministic. See README.md.
"""

from __future__ import annotations

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from xisogen import Content, Image, directory, file, manifest, sha256_file, write  # noqa: E402


def pat(fixture: str, path: str, size: int) -> object:
    return file(path.rsplit("/", 1)[-1], size, Content("pattern", f"{fixture}/{path}"))


def small_tree(fx: str) -> list:
    """A little of everything, sized around sector boundaries."""
    return [
        pat(fx, "default.xbe", 24_576),
        pat(fx, "empty.bin", 0),
        pat(fx, "one-byte.bin", 1),
        pat(fx, "sector-exact.bin", 2048),
        pat(fx, "sector-plus-one.bin", 2049),
        directory("media", [
            pat(fx, "media/intro.xmv", 100_000),
            pat(fx, "media/Title Screen.png", 3_000),
        ]),
        directory("data", [
            pat(fx, "data/level01.dat", 65_536),
            directory("sub", [pat(fx, "data/sub/nested.txt", 123)]),
        ]),
    ]


def deep_tree(fx: str) -> list:
    """Stress the directory parser: depth, wide multi-sector tables, names, empties."""
    # 12 levels of nesting, one file at the bottom.
    leaf: list = [pat(fx, "/".join(f"d{i:02d}" for i in range(12)) + "/bottom.bin", 4_097)]
    for i in reversed(range(12)):
        leaf = [directory(f"d{i:02d}", leaf)]
    # 300 entries in one directory: its table spans several sectors.
    wide = [pat(fx, f"wide/file{i:03d}_{'x' * (i % 30)}.bin", i * 37 % 5_000) for i in range(300)]
    names = [
        pat(fx, "names/" + "a" * 42, 42),  # FATX limit exactly
        pat(fx, "names/" + "b" * 43, 43),  # one over: valid on disc, needs shortening on FATX
        pat(fx, "names/" + "c" * 255, 255),  # longest name XDVDFS allows
        pat(fx, "names/MixedCase.TXT", 10),
        pat(fx, "names/lower.txt", 11),
        pat(fx, "names/_underscore", 12),
        pat(fx, "names/~tilde", 13),
        pat(fx, "names/with space.bin", 14),
        pat(fx, "names/dots.in.name.bin", 15),
    ]
    return leaf + [
        directory("wide", wide),
        directory("names", names),
        directory("empty-dir", []),
        directory("only-empty-subdir", [directory("inner-empty", [])]),
        pat(fx, "default.xbe", 8_192),
    ]


def large_tree(fx: str) -> list:
    """>4 GiB image: the largest file FATX can store, then files whose offsets exceed 2^32.

    a_huge.bin is 4 GiB - 16 KiB, the most the console's FATX lets a file grow to on E:
    (measured with app/fatx-probe v4; docs/formats/fatx-names.md §4b). It was 0xFFFFF800
    until a hardware install showed that 14 KiB more can't be written.
    """
    return [
        file("a_huge.bin", 0xFFFFC000, Content("sparse", f"{fx}/a_huge.bin")),
        pat(fx, "b_after_4gib.bin", 1_048_576),
        pat(fx, "c_tail.bin", 3),
    ]


FIXTURES = {
    "small": ("xiso", small_tree),
    "deep": ("xiso", deep_tree),
    "redump-xgd1": ("xgd1", small_tree),
    "redump-xgd2": ("xgd2", small_tree),
    "redump-xgd3": ("xgd3", small_tree),
    # Tables after their files (each subdirectory's before its parent's), for installs from
    # a sequential stream (Deflate zips, ADR 0009); padding between files.
    "late": ("xiso", small_tree, "tables-last", 16),
    "late-deep": ("xiso", deep_tree, "tables-last", 0),
    "late-xgd3": ("xgd3", small_tree, "tables-last", 16),
    "late-root": ("xiso", deep_tree, "root-last", 0),
}
LARGE = {"large": ("xiso", large_tree)}

# Images bigger than this get no whole-image hash (it would mean reading GBs of holes).
IMAGE_HASH_LIMIT = 64 * 1024 * 1024


def build(name: str, layout_name: str, tree, out: str, order: str = "tables-first",
          gap_sectors: int = 0) -> dict:
    image = Image(name=name, layout=layout_name, root=tree(name), order=order,
                  gap_sectors=gap_sectors)
    iso = os.path.join(out, f"{name}.iso")
    write(image, iso)
    digest = sha256_file(iso) if image.size <= IMAGE_HASH_LIMIT else None
    m = manifest(image, digest)
    with open(os.path.join(out, f"{name}.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(m, f, indent=1, sort_keys=True)
        f.write("\n")
    write_tsv(m, os.path.join(out, f"{name}.tsv"))
    return m


def write_tsv(m: dict, path: str) -> None:
    """The manifest as tab-separated lines, for tests in C (no JSON parser needed).

    layout <layout> <partition_offset>
    root   <sector> <size>
    file   <offset> <size> <sector> <path>    (offset-sorted, as in the JSON)
    dir    <offset> <size> <sector> <path>    (sorted by path)
    """
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(f"# xcat-fixture-manifest {m['version']} (tsv), see tools/fixtures/README.md\n")
        f.write(f"layout\t{m['layout']}\t{m['partition_offset']}\n")
        f.write(f"root\t{m['root']['sector']}\t{m['root']['size']}\n")
        for kind, entries in (("file", m["files"]), ("dir", m["dirs"])):
            for e in entries:
                f.write(f"{kind}\t{e['offset']}\t{e['size']}\t{e['sector']}\t{e['path']}\n")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True, help="output directory (created if missing)")
    ap.add_argument("--large", action="store_true", help="also build the >4 GiB sparse image")
    ap.add_argument("--only", nargs="+", metavar="NAME", help="build only these fixtures")
    args = ap.parse_args(argv)

    table = dict(FIXTURES, **LARGE) if args.large or (args.only and "large" in args.only) else FIXTURES
    names = args.only or list(table)
    unknown = [n for n in names if n not in table]
    if unknown:
        ap.error(f"unknown fixture(s): {', '.join(unknown)}; known: {', '.join({**FIXTURES, **LARGE})}")

    os.makedirs(args.out, exist_ok=True)
    for name in names:
        layout_name, tree, *order = table[name]
        m = build(name, layout_name, tree, args.out, *order)
        print(f"{name}: {layout_name}, {m['image_size']:,} bytes, "
              f"{len(m['files'])} files, {len(m['dirs'])} dirs")
    return 0


if __name__ == "__main__":
    sys.exit(main())
