"""Write a test file for the console's network test (app/net-test) and print its CRC-32.

    python tools/testserver/make_test_file.py --out <dir> [--size 1G]

The content is pseudo-random bytes from a fixed seed: the same size always gives
the same file and CRC-32, on every machine. It is test data only, never a game
(ADR 0006). The script prints the lines to put in E:\\xcat\\net-test.txt.
"""

from __future__ import annotations

import argparse
import os
import random
import re
import sys
import zlib

CHUNK = 1024 * 1024
UNITS = {"": 1, "K": 1024, "M": 1024**2, "G": 1024**3}


def parse_size(text: str) -> int:
    m = re.fullmatch(r"(\d+)([KMG]?)", text.strip().upper())
    if m is None:
        raise argparse.ArgumentTypeError(f"bad size {text!r} (e.g. 1G, 300M, 12345)")
    return int(m.group(1)) * UNITS[m.group(2)]


def write_test_file(path: str, size: int) -> int:
    """Writes `size` bytes to `path` and returns their CRC-32."""
    rnd = random.Random(0x58434154)  # "XCAT"
    crc = 0
    done = 0
    with open(path, "wb") as f:
        while done < size:
            block = rnd.randbytes(min(CHUNK, size - done))
            crc = zlib.crc32(block, crc)
            f.write(block)
            done += len(block)
    return crc


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True, help="directory to write into (the one rangeserver.py serves)")
    ap.add_argument("--size", type=parse_size, default=parse_size("1G"), help="e.g. 1G (default), 300M")
    args = ap.parse_args(argv)
    os.makedirs(args.out, exist_ok=True)
    name = f"xcat-test-{args.size}.bin"
    path = os.path.join(args.out, name)
    crc = write_test_file(path, args.size)
    print(f"wrote {path}: {args.size} bytes, crc32 {crc:08x}")
    print("For E:\\xcat\\net-test.txt (replace <pc-ip> with this PC's LAN address):")
    print(f"url=http://<pc-ip>:8080/{name}")
    print(f"size={args.size}")
    print(f"crc32={crc:08x}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
