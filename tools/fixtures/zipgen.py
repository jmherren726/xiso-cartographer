"""Make zip archives of generated fixtures, for core/tests/test_zip.c and `xcat-cli zip`.

    python tools/fixtures/zipgen.py --fixtures build/fixtures small

For each NAME it writes, next to NAME.iso:

- NAME-stored.zip: the image stored (method 0), with Zip64 sizes in its local header;
- NAME-deflate.zip: the image Deflate-compressed at level 9 (method 8), like
  the owner's archive.org sources (docs/adr/0009-install-from-zip-archives.md);
- NAME-deflate0.zip: Deflate at level 0, i.e. only stored Deflate blocks;

each with a small `readme.txt` before the image, so the image isn't the first entry.
Output is deterministic for a given zlib: fixed timestamps, names and order.
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys
import zipfile

DATE = (2026, 10, 19, 12, 0, 0)
README = b"Synthetic test archive (tools/fixtures/zipgen.py). No game content.\n"


def entry(name: str, method: int) -> zipfile.ZipInfo:
    info = zipfile.ZipInfo(name, date_time=DATE)
    info.compress_type = method
    info.create_system = 0  # MS-DOS: no Unix mode bits, same on every platform
    info.external_attr = 0
    return info


def write_zip(path: str, iso: str, iso_name: str, method: int, level: int | None = None) -> None:
    tmp = path + ".tmp"
    with zipfile.ZipFile(tmp, "w", compresslevel=level) as z:
        z.writestr(entry("readme.txt", zipfile.ZIP_STORED), README)
        info = entry(iso_name, method)
        if level is not None:
            # open() takes the level from the entry, not from the archive. ZipInfo has
            # __slots__: compress_level from Python 3.13, _compresslevel before.
            setattr(info, "compress_level" if hasattr(info, "compress_level") else "_compresslevel", level)
        with open(iso, "rb") as src, z.open(info, "w", force_zip64=True) as dst:
            shutil.copyfileobj(src, dst, 1 << 20)
    os.replace(tmp, path)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fixtures", required=True, help="directory with the generated NAME.iso files")
    ap.add_argument("names", nargs="+", help="fixture names")
    args = ap.parse_args()
    for name in args.names:
        iso = os.path.join(args.fixtures, f"{name}.iso")
        write_zip(os.path.join(args.fixtures, f"{name}-stored.zip"), iso, f"{name}.iso", zipfile.ZIP_STORED)
        write_zip(os.path.join(args.fixtures, f"{name}-deflate.zip"), iso, f"{name}.iso", zipfile.ZIP_DEFLATED, 9)
        write_zip(os.path.join(args.fixtures, f"{name}-deflate0.zip"), iso, f"{name}.iso", zipfile.ZIP_DEFLATED, 0)
    return 0


if __name__ == "__main__":
    sys.exit(main())
