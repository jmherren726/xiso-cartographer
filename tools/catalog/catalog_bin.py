"""Writes catalog.bin (docs/formats/catalog.md, version 1.0).

The pipeline's last step, and the source of the committed fuzz seeds
(tools/fixtures/catalog_seeds.py). Games and releases come in as plain
records; this module sorts them (section 6.1 collation), interns the
strings and lays the file out byte for byte the same on every machine.
It refuses input the console's reader would refuse.
"""
from __future__ import annotations

import dataclasses
import re
import struct
import zlib
from dataclasses import dataclass, field

MAJOR, MINOR = 1, 0
HEADER_SIZE, GAME_SIZE, RELEASE_SIZE = 64, 32, 72
MAX_SIZE = 8 * 1024 * 1024
MAX_COUNT = 65535
MAX_STRING = 255
MAX_VARIANT = 16
VARIANT_RE = re.compile(r"[A-Z0-9_-]{1,16}")


@dataclass(frozen=True)
class Game:
    title: str
    developer: str = ""
    publisher: str = ""
    genres: str = ""
    wikidata: int = 0
    year: int = 0


@dataclass(frozen=True)
class Release:
    game: int  # index into the games list as given to build()
    title_id: int
    variant: str
    name: str
    region: str = ""
    redump_id: int = 0
    size: int = 0
    crc32: int = 0
    md5: bytes = field(default=bytes(16))
    sha1: bytes = field(default=bytes(20))


def collate_key(s: str) -> bytes:
    """Section 6.1: bytes of the UTF-8, with ASCII A-Z folded to a-z."""
    return bytes(c + 32 if 0x41 <= c <= 0x5A else c for c in s.encode("utf-8"))


def _check_string(s: str) -> None:
    b = s.encode("utf-8")
    if len(b) > MAX_STRING or any(c < 0x20 or c == 0x7F for c in b):
        raise ValueError(f"string not allowed by catalog.md section 6: {s!r}")


def _align4(n: int) -> int:
    return (n + 3) & ~3


def build(games: list[Game], releases: list[Release], snapshot: str) -> bytes:
    """Returns the bytes of catalog.bin. Raises ValueError for input catalog.md forbids."""
    if not 1 <= len(games) <= MAX_COUNT or not 1 <= len(releases) <= MAX_COUNT:
        raise ValueError("1-65535 games and releases are required")
    # Section 3 order: title, then Wikidata number; then remap the releases' game indices.
    order = sorted(range(len(games)), key=lambda i: (collate_key(games[i].title), games[i].wikidata))
    new_index = {old: new for new, old in enumerate(order)}
    games = [games[i] for i in order]
    releases = sorted(
        (dataclasses.replace(r, game=new_index[r.game]) for r in releases),
        key=lambda r: (r.title_id, collate_key(r.variant)),
    )
    names = sorted(range(len(releases)), key=lambda i: collate_key(releases[i].name))

    for g in games:
        for s in (g.title, g.developer, g.publisher, g.genres):
            _check_string(s)
        if not g.title or not (g.year == 0 or 1990 <= g.year <= 2100):
            raise ValueError(f"bad game: {g}")
    for a, b in zip(releases, releases[1:]):
        if (a.title_id, collate_key(a.variant)) == (b.title_id, collate_key(b.variant)):
            raise ValueError(f"duplicate release {a.title_id:08X}@{a.variant}")
    for a, b in zip(names, names[1:]):
        if collate_key(releases[a].name) == collate_key(releases[b].name):
            raise ValueError(f"duplicate name {releases[a].name!r}")
    for r in releases:
        for s in (r.variant, r.name, r.region):
            _check_string(s)
        if not VARIANT_RE.fullmatch(r.variant) or not r.name or not 0 < r.title_id < 2**32:
            raise ValueError(f"bad release: {r}")
        if len(r.md5) != 16 or len(r.sha1) != 20:
            raise ValueError(f"bad hashes: {r}")
    if {r.game for r in releases} != set(range(len(games))):
        raise ValueError("every game needs a release")

    # Section 6: offset 0 is "", each distinct string once, in first-use order.
    strings = bytearray(b"\0")
    offsets: dict[str, int] = {"": 0}

    def intern(s: str) -> int:
        if s not in offsets:
            offsets[s] = len(strings)
            strings.extend(s.encode("utf-8") + b"\0")
        return offsets[s]

    snapshot_off = intern(snapshot)
    _check_string(snapshot)
    game_bytes = b"".join(
        struct.pack("<IIIIIHH8x", intern(g.title), intern(g.developer), intern(g.publisher),
                    intern(g.genres), g.wikidata, g.year, 0)
        for g in games)
    release_bytes = b"".join(
        struct.pack("<IIIIIIQI", r.game, r.title_id, intern(r.variant), intern(r.name),
                    intern(r.region), r.redump_id, r.size, r.crc32) + r.md5 + r.sha1
        for r in releases)
    name_bytes = struct.pack(f"<{len(names)}I", *names)

    games_off = HEADER_SIZE
    releases_off = _align4(games_off + len(game_bytes))
    names_off = _align4(releases_off + len(release_bytes))
    strings_off = _align4(names_off + len(name_bytes))
    size = strings_off + len(strings)
    if size > MAX_SIZE:
        raise ValueError("catalog larger than 8 MiB")
    body = bytearray(size)
    body[games_off:games_off + len(game_bytes)] = game_bytes
    body[releases_off:releases_off + len(release_bytes)] = release_bytes
    body[names_off:names_off + len(name_bytes)] = name_bytes
    body[strings_off:] = strings
    body[:60] = struct.pack("<8sHHIIIHHIIIIII8x", b"XCATCAT\0", MAJOR, MINOR, size, len(games),
                            len(releases), GAME_SIZE, RELEASE_SIZE, games_off, releases_off,
                            names_off, strings_off, len(strings), snapshot_off)
    crc = zlib.crc32(bytes(body[64:]), zlib.crc32(bytes(body[:60])))
    body[60:64] = struct.pack("<I", crc)
    return bytes(body)


def read(data: bytes) -> tuple[list[Game], list[Release]]:
    """Reads a catalog this module wrote (for reports and tests; the console's checked
    reader is xcat/catalog.h). Checks the magic, version and CRC only."""
    magic, major, _minor, size = struct.unpack_from("<8sHHI", data, 0)
    crc = zlib.crc32(data[64:], zlib.crc32(data[:60]))
    if magic != b"XCATCAT\0" or major != MAJOR or size != len(data) or crc != struct.unpack_from("<I", data, 60)[0]:
        raise ValueError("not a catalog.bin this reader knows")
    ng, nr, gsize, rsize, goff, roff, _noff, soff = struct.unpack_from("<IIHHIIII", data, 16)

    def s(off: int) -> str:
        start = soff + off
        return data[start:data.index(b"\0", start)].decode("utf-8")

    games = []
    for i in range(ng):
        t, d, p, g, q, y = struct.unpack_from("<IIIIIH", data, goff + i * gsize)
        games.append(Game(s(t), s(d), s(p), s(g), q, y))
    releases = []
    for i in range(nr):
        o = roff + i * rsize
        game, tid, v, n, reg, rid, size, crc32 = struct.unpack_from("<IIIIIIQI", data, o)
        releases.append(Release(game, tid, s(v), s(n), s(reg), rid, size, crc32,
                                data[o + 36:o + 52], data[o + 52:o + 72]))
    return games, releases
