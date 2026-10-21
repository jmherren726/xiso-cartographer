#!/usr/bin/env python3
"""Converts a PNG to a 24-bit BMP that SDL_LoadBMP reads, with the standard library only.

The XBE build runs this on the UI's images (app/ui/ui-assets.mk): nxdk's SDL core loads BMP
without SDL2_image, and the repo keeps the smaller PNG. Only what our assets use is
supported: 8-bit RGB or RGBA, not interlaced; alpha is dropped.

usage: png2bmp.py <in.png> <out.bmp>
"""

import struct
import sys
import zlib

SIGNATURE = b"\x89PNG\r\n\x1a\n"


def read_png(data):
    """Returns (width, height, rows), each row a bytes of RGB triples."""
    if data[:8] != SIGNATURE:
        raise ValueError("not a PNG")
    pos = 8
    width = height = None
    channels = None
    idat = []
    while pos < len(data):
        if pos + 8 > len(data):
            raise ValueError("truncated chunk header")
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if len(body) != length or pos + 12 + length > len(data):
            raise ValueError("truncated chunk")
        (crc,) = struct.unpack(">I", data[pos + 8 + length:pos + 12 + length])
        if zlib.crc32(kind + body) != crc:
            raise ValueError("bad CRC in %r chunk" % kind)
        pos += 12 + length
        if kind == b"IHDR":
            width, height, depth, color, _comp, _filt, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8 or color not in (2, 6) or interlace != 0:
                raise ValueError("only 8-bit RGB/RGBA, not interlaced, is supported")
            channels = 3 if color == 2 else 4
        elif kind == b"IDAT":
            idat.append(body)
        elif kind == b"IEND":
            break
    if width is None or not idat:
        raise ValueError("no IHDR or IDAT")
    raw = zlib.decompress(b"".join(idat))
    stride = width * channels
    if len(raw) != height * (stride + 1):
        raise ValueError("image data has the wrong size")
    rows = []
    prev = bytearray(stride)
    for y in range(height):
        start = y * (stride + 1)
        kind = raw[start]
        line = bytearray(raw[start + 1:start + 1 + stride])
        unfilter(kind, line, prev, channels)
        rgb = line if channels == 3 else bytearray(b for i, b in enumerate(line) if i % 4 != 3)
        rows.append(bytes(rgb))
        prev = line
    return width, height, rows


def unfilter(kind, line, prev, bpp):
    """Undoes PNG filter `kind` on line, in place (PNG spec section 9)."""
    if kind == 0:
        return
    for i in range(len(line)):
        a = line[i - bpp] if i >= bpp else 0
        b = prev[i]
        c = prev[i - bpp] if i >= bpp else 0
        if kind == 1:
            pred = a
        elif kind == 2:
            pred = b
        elif kind == 3:
            pred = (a + b) // 2
        elif kind == 4:
            p = a + b - c
            pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
            pred = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
        else:
            raise ValueError("unknown filter type %d" % kind)
        line[i] = (line[i] + pred) & 0xFF


def write_bmp(width, height, rows):
    """A BITMAPINFOHEADER BMP, 24 bits, rows bottom-up and padded to 4 bytes."""
    pad = (4 - (width * 3) % 4) % 4
    pixels = bytearray()
    for row in reversed(rows):
        for i in range(0, len(row), 3):
            pixels += bytes((row[i + 2], row[i + 1], row[i]))
        pixels += b"\0" * pad
    header = struct.pack("<2sIHHI", b"BM", 54 + len(pixels), 0, 0, 54)
    info = struct.pack("<IiiHHIIiiII", 40, width, height, 1, 24, 0, len(pixels), 2835, 2835, 0, 0)
    return header + info + bytes(pixels)


def main(argv):
    if len(argv) != 3:
        sys.stderr.write(__doc__)
        return 2
    with open(argv[1], "rb") as f:
        width, height, rows = read_png(f.read())
    with open(argv[2], "wb") as f:
        f.write(write_bmp(width, height, rows))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
