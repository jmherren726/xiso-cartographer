"""Writes the catalog fuzz seeds (core/tests/fuzz-seeds/catalog/) with the pipeline's
writer (tools/catalog/catalog_bin.py). Placeholder games only: no real titles.

Usage: python3 -I tools/fixtures/catalog_seeds.py core/tests/fuzz-seeds/catalog
"""
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "catalog"))
import catalog_bin as cb  # noqa: E402


def recrc(b: bytearray) -> bytes:
    b[60:64] = struct.pack("<I", zlib.crc32(bytes(b[64:]), zlib.crc32(bytes(b[:60]))))
    return bytes(b)


def main() -> None:
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    games = [
        cb.Game("Alpha Racer", "Studio One", "Pub A", "racing", 1001, 2002),
        cb.Game("bravo Tactics"),
        cb.Game("Charlie Quest é€", "Studio Two, Studio Three", "Pub B",
                "action, adventure", 1003, 2004),
    ]
    releases = [
        cb.Release(0, 0x41410001, "US", "Alpha Racer (USA)", "USA", 12, 7825162240, 0x1234,
                   bytes(range(16)), bytes(range(20))),
        cb.Release(0, 0x41410001, "EU", "Alpha Racer (Europe)", "Europe", 11, 7825162240),
        cb.Release(2, 0x43430010, "US-EU", "Charlie Quest (USA, Europe)", "USA, Europe", 31),
        cb.Release(1, 0x4D530004, "JP", "Bravo Tactics (Japan)", "Japan"),
    ]
    good = cb.build(games, releases, "seed 2026-10-20")
    one = cb.build([cb.Game("A")], [cb.Release(0, 1, "US", "A (USA)")], "")

    # A newer minor (1.1): records 4 bytes longer, reserved bytes used.
    newer = bytearray(good)
    ng, nr = struct.unpack_from("<II", newer, 16)
    g_off, r_off, n_off, s_off, s_size = struct.unpack_from("<IIIII", newer, 28)
    games_part = b"".join(newer[g_off + 32 * i:g_off + 32 * (i + 1)] + b"\x01\0\0\0" for i in range(ng))
    rel_part = b"".join(newer[r_off + 72 * i:r_off + 72 * (i + 1)] + b"\x02\0\0\0" for i in range(nr))
    names_part = newer[n_off:n_off + 4 * nr]
    strings_part = newer[s_off:s_off + s_size]
    g2 = 64
    r2 = g2 + len(games_part)
    n2 = r2 + len(rel_part)
    s2 = n2 + len(names_part)
    newer = bytearray(newer[:64]) + games_part + rel_part + names_part + strings_part
    struct.pack_into("<HHI", newer, 8, 1, 1, len(newer))
    struct.pack_into("<HHIIII", newer, 24, 36, 76, g2, r2, n2, s2)
    newer[52] = 7

    unsorted = bytearray(good)
    _, _, n_off, _, _ = struct.unpack_from("<IIIII", unsorted, 28)
    unsorted[n_off:n_off + 8] = unsorted[n_off + 4:n_off + 8] + unsorted[n_off:n_off + 4]

    seeds = {
        "good": good,
        "one": one,
        "newer-minor": recrc(newer),
        "names-unsorted": recrc(unsorted),
        "major-2": recrc(bytearray(good[:8]) + b"\2\0" + good[10:]),
        "header-only": good[:64],
    }
    for name, data in seeds.items():
        with open(os.path.join(out, name + ".seed"), "wb") as f:
            f.write(data)


if __name__ == "__main__":
    main()
