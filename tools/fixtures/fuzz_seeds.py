"""Make fuzz seeds for core/tests/fuzz_xdvdfs.c from generated fixtures.

    python tools/fixtures/fuzz_seeds.py --fixtures build/fixtures --out build/fuzz-seeds small deep

A seed is the harness's input layout: u32 root-sector delta (root sector - 33),
u32 root table size, then the image bytes from the root table to the end of the
last directory table (file data is left out; the harness pads with zeros).
Only trimmed xiso fixtures (partition at 0) can be used.
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import sys

SECTOR = 2048
FIRST_ROOT_SECTOR = 33
MAX_DELTA = 4096


def make_seed(fixtures: str, name: str) -> bytes:
    with open(os.path.join(fixtures, f"{name}.json"), encoding="utf-8") as f:
        m = json.load(f)
    if m["partition_offset"] != 0:
        raise SystemExit(f"{name}: only xiso fixtures (partition at 0) make seeds")
    root = m["root"]
    delta = root["sector"] - FIRST_ROOT_SECTOR
    if not 0 <= delta < MAX_DELTA:
        raise SystemExit(f"{name}: root sector {root['sector']} outside the harness range")
    start = root["sector"] * SECTOR
    end = max([start + root["size"]] + [d["offset"] + d["size"] for d in m["dirs"]])
    with open(os.path.join(fixtures, f"{name}.iso"), "rb") as f:
        f.seek(start)
        tables = f.read(end - start)
    return struct.pack("<II", delta, root["size"]) + tables


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fixtures", required=True, help="directory with <name>.iso and <name>.json")
    ap.add_argument("--out", required=True, help="output directory for <name>.seed")
    ap.add_argument("names", nargs="+", help="fixture names (xiso layout only)")
    args = ap.parse_args(argv)
    os.makedirs(args.out, exist_ok=True)
    for name in args.names:
        seed = make_seed(args.fixtures, name)
        with open(os.path.join(args.out, f"{name}.seed"), "wb") as f:
            f.write(seed)
        print(f"{name}.seed: {len(seed):,} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
