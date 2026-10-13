"""Deterministic XDVDFS (Xbox disc) image writer for test fixtures.

Builds an image from an in-memory tree of dummy files, so tests never need
real (copyrighted) game images. The on-disc structures follow the layout that
extract-xiso reads and writes (third_party/nxdk/tools/extract-xiso):

- Volume descriptor at partition offset 0x10000 (sector 32): magic, root
  directory sector (u32), root directory size (u32), FILETIME (u64), 0x7C8
  unused bytes, magic again.
- Directory tables: binary search trees of entries. Each entry is
  left/right child offsets (u16, in 4-byte units from the table start, 0 = none),
  start sector (u32), size (u32), attributes (u8), name length (u8), name,
  padded with 0xFF to a 4-byte boundary. Entries never cross a sector boundary;
  unused table space is 0xFF. A directory's size is its table size rounded up
  to whole sectors; an empty directory is one 0xFF-filled sector.
- Names are ordered with extract-xiso's key: bytewise, with only a-z folded
  to upper case.
- Full-disc (Redump) layouts put the game partition at a fixed offset; sector
  numbers inside it are relative to the partition start.

All integers are little-endian. File data is never stored in memory as a whole:
content is generated in chunks, and zero regions are left as sparse holes.
"""

from __future__ import annotations

import hashlib
import os
import struct
import sys
from dataclasses import dataclass, field
from typing import BinaryIO, Iterator

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
VOLUME_DESCRIPTOR_OFFSET = 0x10000
ROOT_TABLE_SECTOR = 0x108  # where extract-xiso puts the root table
IMAGE_MODULUS = 0x10000  # extract-xiso pads images to a 64 KiB multiple
ENTRY_HEADER = 14
PAD = 0xFF
ATTR_DIRECTORY = 0x10
ATTR_ARCHIVE = 0x20
MAX_FILE_SIZE = 0xFFFFFFFF  # 32-bit size field
MAX_TABLE_SIZE = 0xFFFF * 4  # child offsets are u16 counts of 4-byte units
# Fixed timestamp keeps output deterministic: 2001-11-15 00:00:00 UTC as FILETIME.
FILETIME = 126449280000000000

# Game partition offset for each layout.
LAYOUTS = {
    "xiso": 0,
    "xgd1": 0x18300000,
    "xgd2": 0x0FD90000,
    "xgd3": 0x02080000,
}
# Full-disc layouts are padded (as a sparse hole) to the size of a full dump.
# Readers probe every known partition offset, and a real full-disc image is
# always longer than all of them; a short fixture would fail those probes.
FULL_DISC_SIZE = {
    "xgd1": 7_825_162_240,
    "xgd2": 7_838_695_424,
    "xgd3": 8_738_846_720,
}

CHUNK = 1 << 20


# --- content -----------------------------------------------------------------


@dataclass(frozen=True)
class Content:
    """How a dummy file's bytes are generated.

    kind "pattern": every byte is pseudo-random, derived from `seed`.
    kind "sparse":  zeros, except `edge` pattern bytes at the start and end.
    """

    kind: str
    seed: str
    edge: int = 4096

    def regions(self, size: int) -> Iterator[tuple[int, int]]:
        """Yield (offset, length) of the non-zero regions."""
        if self.kind == "pattern":
            if size:
                yield 0, size
        elif self.kind == "sparse":
            if size <= 2 * self.edge:
                if size:
                    yield 0, size
            else:
                yield 0, self.edge
                yield size - self.edge, self.edge
        else:
            raise ValueError(f"unknown content kind {self.kind!r}")

    def pattern(self, offset: int, length: int) -> bytes:
        """Pattern bytes [offset, offset+length) of the file's 'pattern' stream.

        Block i (64 bytes) is BLAKE2b-512 of "<seed>:<i>". Defined this way so
        any range can be produced independently and reproduced in any language.
        """
        out = bytearray()
        first = offset // 64
        last = (offset + length + 63) // 64
        for i in range(first, last):
            out += hashlib.blake2b(f"{self.seed}:{i}".encode(), digest_size=64).digest()
        start = offset - first * 64
        return bytes(out[start : start + length])

    def chunks(self, size: int) -> Iterator[tuple[int, bytes]]:
        """Yield (offset, bytes) for the non-zero data, in order, in bounded chunks."""
        for start, length in self.regions(size):
            pos = start
            while pos < start + length:
                n = min(CHUNK, start + length - pos)
                yield pos, self.pattern(pos, n)
                pos += n

    def sha256(self, size: int) -> str:
        """SHA-256 of the whole file, holes included, without materialising it."""
        h = hashlib.sha256()
        pos = 0
        zeros = bytes(CHUNK)
        for off, data in self.chunks(size):
            while pos < off:
                n = min(CHUNK, off - pos)
                h.update(zeros[:n])
                pos += n
            h.update(data)
            pos += len(data)
        while pos < size:
            n = min(CHUNK, size - pos)
            h.update(zeros[:n])
            pos += n
        return h.hexdigest()


# --- tree --------------------------------------------------------------------


@dataclass
class Node:
    name: str
    size: int = 0  # files only
    content: Content | None = None  # files only
    children: list["Node"] | None = None  # directories only
    # Assigned during layout:
    sector: int = 0
    table: bytes = b""  # directories only

    @property
    def is_dir(self) -> bool:
        return self.children is not None


def file(name: str, size: int, content: Content) -> Node:
    return Node(name=name, size=size, content=content)


def directory(name: str, children: list[Node]) -> Node:
    return Node(name=name, children=children)


def sort_key(name: str) -> bytes:
    """extract-xiso's avl_compare_key: bytewise, a-z folded to upper case."""
    return bytes(b - 32 if 0x61 <= b <= 0x7A else b for b in name.encode("ascii"))


def check_name(name: str) -> None:
    raw = name.encode("ascii")  # non-ASCII would make the ordering ambiguous
    if not 1 <= len(raw) <= 255:
        raise ValueError(f"name length must be 1..255: {name!r}")
    if name in (".", "..") or "/" in name or "\\" in name or any(b < 0x20 for b in raw):
        raise ValueError(f"invalid name: {name!r}")


# --- directory tables ----------------------------------------------------------


@dataclass
class _Slot:
    node: Node
    left: "_Slot | None" = None
    right: "_Slot | None" = None
    offset: int = 0


def _balanced(nodes: list[Node]) -> _Slot | None:
    if not nodes:
        return None
    mid = len(nodes) // 2
    return _Slot(nodes[mid], _balanced(nodes[:mid]), _balanced(nodes[mid + 1 :]))


def _preorder(slot: _Slot | None) -> Iterator[_Slot]:
    if slot:
        yield slot
        yield from _preorder(slot.left)
        yield from _preorder(slot.right)


def _entry_len(node: Node) -> int:
    n = ENTRY_HEADER + len(node.name)
    return (n + 3) & ~3


def build_table(children: list[Node]) -> bytes:
    """Serialise one directory's entries; child sectors and sizes must be final."""
    if not children:
        return bytes([PAD]) * SECTOR
    ordered = sorted(children, key=lambda n: sort_key(n.name))
    for a, b in zip(ordered, ordered[1:]):
        if sort_key(a.name) == sort_key(b.name):
            raise ValueError(f"names collide case-insensitively: {a.name!r}, {b.name!r}")
    root = _balanced(ordered)
    slots = list(_preorder(root))  # root first, so it sits at offset 0
    pos = 0
    for s in slots:
        n = _entry_len(s.node)
        if pos // SECTOR != (pos + n - 1) // SECTOR:
            pos = (pos // SECTOR + 1) * SECTOR  # never cross a sector boundary
        s.offset = pos
        pos += n
    if pos > MAX_TABLE_SIZE:
        raise ValueError(f"directory table too large ({pos} bytes)")
    table = bytearray([PAD]) * (((pos + SECTOR - 1) // SECTOR) * SECTOR)
    for s in slots:
        node = s.node
        size = len(node.table) if node.is_dir else node.size
        attrs = ATTR_DIRECTORY if node.is_dir else ATTR_ARCHIVE
        name = node.name.encode("ascii")
        left = s.left.offset // 4 if s.left else 0
        right = s.right.offset // 4 if s.right else 0
        entry = struct.pack("<HHIIBB", left, right, node.sector, size, attrs, len(name)) + name
        table[s.offset : s.offset + len(entry)] = entry
    return bytes(table)


def _table_size(children: list[Node]) -> int:
    """Table size before sectors are known (entry sizes don't depend on them)."""
    for c in children:
        c.sector = 0
        if c.is_dir and not c.table:
            c.table = bytes(SECTOR)  # placeholder; only its length matters here
    return len(build_table(children))


# --- image -------------------------------------------------------------------


@dataclass
class Image:
    name: str
    layout: str
    root: list[Node]
    entries: list[dict] = field(default_factory=list)
    size: int = 0
    root_sector: int = 0
    root_size: int = 0


def _walk(nodes: list[Node], prefix: str = "") -> Iterator[tuple[str, Node]]:
    for n in sorted(nodes, key=lambda n: sort_key(n.name)):
        check_name(n.name)
        path = f"{prefix}{n.name}"
        yield path, n
        if n.is_dir:
            yield from _walk(n.children, path + "/")


def layout(image: Image) -> None:
    """Assign sectors: all directory tables first (root at 0x108), then files."""
    nodes = list(_walk(image.root))
    dirs = [n for _, n in nodes if n.is_dir]
    files = [(p, n) for p, n in nodes if not n.is_dir]
    for _, n in files:
        if not 0 <= n.size <= MAX_FILE_SIZE:
            raise ValueError(f"{n.name}: file size must fit in 32 bits")

    # Sizes first (bottom-up, since a directory entry records its child table size).
    def size_tables(children: list[Node]) -> None:
        for c in children:
            if c.is_dir:
                size_tables(c.children)
                c.table = bytes(_table_size(c.children))

    size_tables(image.root)
    image.root_size = _table_size(image.root)

    sector = ROOT_TABLE_SECTOR
    image.root_sector = sector
    sector += image.root_size // SECTOR
    for d in dirs:
        d.sector = sector
        sector += len(d.table) // SECTOR
    for _, f in files:
        f.sector = sector
        sector += (f.size + SECTOR - 1) // SECTOR

    # Now every sector is known: build the real tables, deepest first.
    def fill(children: list[Node]) -> None:
        for c in children:
            if c.is_dir:
                fill(c.children)
                c.table = build_table(c.children)

    fill(image.root)
    partition = LAYOUTS[image.layout]
    end = partition + sector * SECTOR
    image.size = (end + IMAGE_MODULUS - 1) // IMAGE_MODULUS * IMAGE_MODULUS
    if image.layout in FULL_DISC_SIZE:
        if image.size > FULL_DISC_SIZE[image.layout]:
            raise ValueError("content doesn't fit in a full-disc image")
        image.size = FULL_DISC_SIZE[image.layout]

    image.entries = []
    for path, n in nodes:
        e = {"path": path, "type": "dir" if n.is_dir else "file", "sector": n.sector,
             "offset": partition + n.sector * SECTOR,
             "size": len(n.table) if n.is_dir else n.size}
        if not n.is_dir:
            e["sha256"] = n.content.sha256(n.size)
        image.entries.append(e)


def _make_sparse(f: BinaryIO) -> None:
    """Best effort: mark the file sparse on Windows (NTFS), so holes cost no disk."""
    if sys.platform != "win32":
        return  # POSIX filesystems create holes on seek+write/truncate
    import ctypes
    import msvcrt
    from ctypes import wintypes

    FSCTL_SET_SPARSE = 0x000900C4
    handle = msvcrt.get_osfhandle(f.fileno())
    returned = wintypes.DWORD()
    ctypes.windll.kernel32.DeviceIoControl(
        wintypes.HANDLE(handle), FSCTL_SET_SPARSE, None, 0, None, 0, ctypes.byref(returned), None
    )


def write(image: Image, path: str) -> None:
    """Write the image. Holes (zeros) are never written, so large images stay sparse."""
    layout(image)
    partition = LAYOUTS[image.layout]
    root_table = build_table(image.root)
    with open(path, "wb") as f:
        _make_sparse(f)
        vd = MAGIC + struct.pack("<IIQ", image.root_sector, image.root_size, FILETIME)
        vd += bytes(0x7C8) + MAGIC
        assert len(vd) == SECTOR
        f.seek(partition + VOLUME_DESCRIPTOR_OFFSET)
        f.write(vd)
        f.seek(partition + image.root_sector * SECTOR)
        f.write(root_table)
        for _, n in _walk(image.root):
            if n.is_dir:
                f.seek(partition + n.sector * SECTOR)
                f.write(n.table)
            else:
                base = partition + n.sector * SECTOR
                for off, data in n.content.chunks(n.size):
                    f.seek(base + off)
                    f.write(data)
        # Extend to full size with one byte at the end, not truncate(): on Windows,
        # truncate() grows a file by physically writing zeros, defeating sparseness.
        end = f.seek(0, os.SEEK_END)
        if end < image.size:
            f.seek(image.size - 1)
            f.write(b"\0")


def manifest(image: Image, image_sha256: str | None) -> dict:
    files = sorted((e for e in image.entries if e["type"] == "file"),
                   key=lambda e: (e["offset"], e["path"]))
    dirs = sorted((e for e in image.entries if e["type"] == "dir"), key=lambda e: e["path"])
    return {
        "format": "xcat-fixture-manifest",
        "version": 1,
        "name": image.name,
        "layout": image.layout,
        "partition_offset": LAYOUTS[image.layout],
        "image_size": image.size,
        "image_sha256": image_sha256,
        "root": {"sector": image.root_sector, "size": image.root_size},
        "files": files,
        "dirs": dirs,
    }


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(CHUNK), b""):
            h.update(block)
    return h.hexdigest()


if __name__ == "__main__":
    sys.exit("xisogen is a library; run fixtures.py to build the fixture set.")
