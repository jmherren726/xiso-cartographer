"""Writes the zip fuzz seeds (core/tests/fuzz-seeds/zip/): small archives covering
docs/formats/zip.md sections 2-6, Zip64 ones built byte by byte.

Usage: python3 tools/fixtures/zip_seeds.py core/tests/fuzz-seeds/zip
"""

from __future__ import annotations

import io
import os
import struct
import sys
import zipfile
import zlib

DATE = (2026, 10, 19, 12, 0, 0)


def with_zipfile(entries, comment=b"", force_zip64=False) -> bytes:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        for name, data, method in entries:
            info = zipfile.ZipInfo(name, date_time=DATE)
            info.compress_type = method
            info.create_system = 0
            with z.open(info, "w", force_zip64=force_zip64) as f:
                f.write(data)
        z.comment = comment
    return buf.getvalue()


def zip64_by_hand(name: bytes, data: bytes, method: int) -> bytes:
    """One entry; all-ones sizes and offset in the central directory, Zip64 end records."""
    crc = zlib.crc32(data)
    stored = data if method == 0 else zlib.compress(data, 9)[2:-4]
    local = struct.pack("<IHHHHHIIIHH", 0x04034B50, 45, 0, method, 0, 0, crc,
                        0xFFFFFFFF, 0xFFFFFFFF, len(name), 20)
    local += name + struct.pack("<HHQQ", 1, 16, len(data), len(stored)) + stored
    extra = struct.pack("<HHQQQ", 1, 24, len(data), len(stored), 0)
    central = struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, 45, 45, 0, method, 0, 0, crc,
                          0xFFFFFFFF, 0xFFFFFFFF, len(name), len(extra), 0, 0, 0, 0, 0xFFFFFFFF)
    central += name + extra
    cd_at = len(local)
    rec_at = cd_at + len(central)
    rec = struct.pack("<IQHHIIQQQQ", 0x06064B50, 44, 45, 45, 0, 0, 1, 1, len(central), cd_at)
    loc = struct.pack("<IIQI", 0x07064B50, 0, rec_at, 1)
    eocd = struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, 0xFFFF, 0xFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0)
    return local + central + rec + loc + eocd


def main() -> int:
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    image = bytes(range(256)) * 8
    seeds = {
        "stored": with_zipfile([("game.iso", image, zipfile.ZIP_STORED)]),
        "deflate": with_zipfile([("game.iso", image, zipfile.ZIP_DEFLATED)]),
        "two-entries": with_zipfile([("readme.txt", b"hi\n", zipfile.ZIP_STORED),
                                     ("Game.XISO", image, zipfile.ZIP_DEFLATED)],
                                    comment=b"a comment PK\x05\x06"),
        "local-zip64": with_zipfile([("game.iso", image, zipfile.ZIP_STORED)], force_zip64=True),
        "two-images": with_zipfile([("a.iso", b"a", zipfile.ZIP_STORED),
                                    ("b.iso", b"b", zipfile.ZIP_STORED)]),
        "empty": with_zipfile([]),
        "zip64-stored": zip64_by_hand(b"game.iso", image, 0),
        "zip64-deflate": zip64_by_hand(b"game.iso", image, 8),
    }
    for k, v in seeds.items():
        with open(os.path.join(out, k + ".seed"), "wb") as f:
            f.write(v)
    return 0


if __name__ == "__main__":
    sys.exit(main())
