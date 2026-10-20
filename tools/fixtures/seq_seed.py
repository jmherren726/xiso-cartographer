#!/usr/bin/env python3
"""Writes a fuzz_seq seed: a fixture image with two bytes appended (core/tests/fuzz_seq.c).

The last byte sets the disk size, the byte before it a power cut and resume (stream-install.md
§7). Bytes past the image's last sector belong to no file or table, so the image itself
doesn't change.

    seq_seed.py <image> <seed> <cut> <disk>
"""
import sys


def main(argv: list[str]) -> int:
    if len(argv) != 5:
        print(__doc__, file=sys.stderr)
        return 2
    data = open(argv[1], "rb").read() + bytes([int(argv[3], 0), int(argv[4], 0)])
    with open(argv[2], "wb") as f:
        f.write(data)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
