#!/usr/bin/env python3
"""Puts a title image into an XBE, as the section the dashboards show, with the standard library only.

The XBE build runs this on the app's default.xbe after nxdk's cxbe made it (app/Makefile),
since cxbe can't add one. The image becomes a 128 x 128 DXT1 texture in an XPR0 resource, in
a new section named $$XTIMAGE at the end of the XBE, as docs/formats/xbe-title-image.md says.
The XBE is changed in place, and only the way that document allows: one section header, its
name and its page counts go in the headers' unused room, and the section's bytes go at the end.

usage: xbe_title_image.py <default.xbe> <image.png>
"""

import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from png2bmp import read_png  # noqa: E402

SECTION_NAME = b"$$XTIMAGE"
SIZE = 128                      # the image's width and height (§2)
PAGE = 0x1000
XPR_HEADER_SIZE = 0x800         # §2: the XPR0 header, padded
TEXTURE_COMMON = 0x00040001     # §2: a texture resource, one reference
# §2: DMA context A, border from colour, 2D, DXT1, one mipmap level, 2^7 x 2^7.
TEXTURE_FORMAT = 0x29 | (0x0C << 8) | (1 << 16) | (7 << 20) | (7 << 24)
FLAG_INSERTED_FILE = 0x8        # §3: not preloaded; nothing on the console loads it
SECTION_HEADER = struct.Struct("<9I20s")


def rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def expand565(c):
    r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
    return (r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)


def palette(c0, c1):
    """The four colours of a four-colour DXT1 block (c0 > c1), as RGB tuples."""
    a, b = expand565(c0), expand565(c1)
    return [a, b, tuple((2 * x + y) // 3 for x, y in zip(a, b)),
            tuple((x + 2 * y) // 3 for x, y in zip(a, b))]


def encode_block(pixels):
    """One 4 x 4 block (16 RGB tuples, row by row) as 8 bytes of DXT1 in four-colour mode."""
    lo = tuple(min(p[i] for p in pixels) for i in range(3))
    hi = tuple(max(p[i] for p in pixels) for i in range(3))
    c0, c1 = rgb565(*hi), rgb565(*lo)
    if c0 < c1:
        c0, c1 = c1, c0
    if c0 == c1:
        return struct.pack("<HHI", c0, c1, 0)  # one colour: every pixel takes c0
    pal = palette(c0, c1)
    bits = 0
    for i, p in enumerate(pixels):
        best = min(range(4), key=lambda k: sum((p[j] - pal[k][j]) ** 2 for j in range(3)))
        bits |= best << (2 * i)
    return struct.pack("<HHI", c0, c1, bits)


def encode_dxt1(width, height, rows):
    """An RGB image (rows of RGB triples) as DXT1 blocks, left to right, top to bottom."""
    if width % 4 or height % 4:
        raise ValueError("the image's sides must be multiples of 4")
    out = bytearray()
    for by in range(0, height, 4):
        for bx in range(0, width, 4):
            px = [tuple(rows[by + y][3 * (bx + x):3 * (bx + x) + 3]) for y in range(4) for x in range(4)]
            out += encode_block(px)
    return bytes(out)


def xpr_texture(dxt1):
    """An XPR0 resource holding one 128 x 128 DXT1 texture (§2)."""
    header = struct.pack("<4sII", b"XPR0", XPR_HEADER_SIZE + len(dxt1), XPR_HEADER_SIZE)
    header += struct.pack("<5I", TEXTURE_COMMON, 0, 0, TEXTURE_FORMAT, 0)
    header += struct.pack("<I", 0xFFFFFFFF)
    return header + bytes(XPR_HEADER_SIZE - len(header)) + dxt1


def section_names(xbe, base, count, headers_at):
    names = []
    for i in range(count):
        f = SECTION_HEADER.unpack_from(xbe, headers_at + i * SECTION_HEADER.size)
        at = f[5] - base
        end = xbe.index(b"\0", at)
        names.append(xbe[at:end])
    return names


def add_section(xbe, name, data, flags):
    """xbe with one more section (§3): its header, name and page counts in the headers' unused
    room, its bytes at the end of the file and of the image in memory."""
    if len(xbe) < 0x178 or xbe[:4] != b"XBEH":
        raise ValueError("not an XBE")
    base, headers_size, image_size = struct.unpack_from("<3I", xbe, 0x104)
    count, headers_addr = struct.unpack_from("<2I", xbe, 0x11C)
    headers_at = headers_addr - base
    if headers_size > len(xbe) or headers_at < 0x178 or \
            headers_at + count * SECTION_HEADER.size > headers_size:
        raise ValueError("the XBE's section headers aren't inside its headers")
    sections = [SECTION_HEADER.unpack_from(xbe, headers_at + i * SECTION_HEADER.size)
                for i in range(count)]
    if name in section_names(xbe, base, count, headers_at):
        raise ValueError("the XBE already has a %s section" % name.decode())
    # The headers may grow up to the first section, in the file and in memory.
    room = min(min(s[3] for s in sections), min(s[1] for s in sections) - base)
    # Appended to the headers: the section headers again with the new one, its name, and its
    # head and tail page counts (two 16-bit words, zero).
    new_headers_at = (headers_size + 3) & ~3
    names_at = new_headers_at + (count + 1) * SECTION_HEADER.size
    pages_at = (names_at + len(name) + 1 + 1) & ~1
    end = pages_at + 4
    if end > room:
        raise ValueError("no room in the XBE's headers for another section")
    vaddr = (max(s[1] + s[2] for s in sections) + PAGE - 1) & ~(PAGE - 1)
    raw = (len(xbe) + PAGE - 1) & ~(PAGE - 1)
    digest = hashlib.sha1(struct.pack("<I", len(data)) + data).digest()
    new = SECTION_HEADER.pack(flags, vaddr, len(data), raw, len(data), base + names_at, 0,
                              base + pages_at, base + pages_at + 2, digest)
    out = bytearray(xbe)
    out[headers_size:end] = bytes(end - headers_size)
    n = count * SECTION_HEADER.size
    out[new_headers_at:new_headers_at + n] = xbe[headers_at:headers_at + n]
    out[new_headers_at + n:names_at] = new
    out[names_at:names_at + len(name) + 1] = name + b"\0"
    struct.pack_into("<3I", out, 0x104, base, end, vaddr + len(data) - base)
    struct.pack_into("<2I", out, 0x11C, count + 1, base + new_headers_at)
    out += bytes(raw - len(out)) + data
    out += bytes(-len(out) % PAGE)
    return bytes(out)


def title_image(png):
    """A 128 x 128 PNG as the $$XTIMAGE section's bytes."""
    width, height, rows = read_png(png)
    if (width, height) != (SIZE, SIZE):
        raise ValueError("the title image must be %d x %d, not %d x %d" % (SIZE, SIZE, width, height))
    return xpr_texture(encode_dxt1(width, height, rows))


def main(argv):
    if len(argv) != 3:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    try:
        with open(argv[1], "rb") as f:
            xbe = f.read()
        with open(argv[2], "rb") as f:
            data = title_image(f.read())
        out = add_section(xbe, SECTION_NAME, data, FLAG_INSERTED_FILE)
    except (OSError, ValueError) as e:
        print("xbe_title_image.py: %s" % e, file=sys.stderr)
        return 1
    tmp = argv[1] + ".tmp"
    with open(tmp, "wb") as f:
        f.write(out)
    os.replace(tmp, argv[1])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
