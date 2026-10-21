"""Tests for png2bmp.py: PNGs made here with every filter type, read back from the BMP."""

import os
import struct
import sys
import unittest
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import png2bmp  # noqa: E402


def chunk(kind, body):
    return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))


def filtered(kind, line, prev, bpp):
    """Applies PNG filter `kind` to line (the inverse of png2bmp.unfilter)."""
    out = bytearray(len(line))
    for i in range(len(line)):
        a = line[i - bpp] if i >= bpp else 0
        b = prev[i]
        c = prev[i - bpp] if i >= bpp else 0
        if kind == 0:
            pred = 0
        elif kind == 1:
            pred = a
        elif kind == 2:
            pred = b
        elif kind == 3:
            pred = (a + b) // 2
        else:
            p = a + b - c
            pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
            pred = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
        out[i] = (line[i] - pred) & 0xFF
    return bytes(out)


def make_png(width, height, channels, pixel):
    color = 2 if channels == 3 else 6
    raw = bytearray()
    prev = bytes(width * channels)
    for y in range(height):
        line = bytes(v for x in range(width) for v in pixel(x, y)[:channels])
        kind = y % 5  # every filter type
        raw += bytes((kind,)) + filtered(kind, line, prev, channels)
        prev = line
    ihdr = struct.pack(">IIBBBBB", width, height, 8, color, 0, 0, 0)
    return (png2bmp.SIGNATURE + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(bytes(raw)))
            + chunk(b"IEND", b""))


def pixel(x, y):
    return ((x * 37 + y * 11) & 0xFF, (x * 5 + y * 91) & 0xFF, (x ^ y) * 13 & 0xFF, 128)


def bmp_pixel(bmp, width, height, x, y):
    stride = (width * 3 + 3) & ~3
    at = 54 + (height - 1 - y) * stride + x * 3
    b, g, r = bmp[at:at + 3]
    return (r, g, b)


class Png2BmpTest(unittest.TestCase):
    def check(self, width, height, channels):
        bmp = png2bmp.write_bmp(*png2bmp.read_png(make_png(width, height, channels, pixel)))
        magic, size, _r1, _r2, offset = struct.unpack("<2sIHHI", bmp[:14])
        hsize, w, h, planes, bits = struct.unpack("<IiiHH", bmp[14:30])
        self.assertEqual((magic, size, offset), (b"BM", len(bmp), 54))
        self.assertEqual((hsize, w, h, planes, bits), (40, width, height, 1, 24))
        for y in range(height):
            for x in range(width):
                self.assertEqual(bmp_pixel(bmp, width, height, x, y), pixel(x, y)[:3], (x, y))

    def test_rgb_every_filter(self):
        self.check(7, 10, 3)  # 21-byte rows: padded to 24

    def test_rgba_alpha_dropped(self):
        self.check(4, 6, 4)

    def test_rejects(self):
        good = make_png(2, 2, 3, pixel)
        with self.assertRaises(ValueError):
            png2bmp.read_png(b"GIF89a" + good[6:])
        bad_crc = bytearray(good)
        bad_crc[30] ^= 1  # inside IHDR's CRC
        with self.assertRaises(ValueError):
            png2bmp.read_png(bytes(bad_crc))
        with self.assertRaises(ValueError):
            png2bmp.read_png(good[:40])
        ihdr16 = struct.pack(">IIBBBBB", 2, 2, 16, 2, 0, 0, 0)
        with self.assertRaises(ValueError):
            png2bmp.read_png(png2bmp.SIGNATURE + chunk(b"IHDR", ihdr16) + chunk(b"IEND", b""))

    def test_the_bundled_background(self):
        path = os.path.join(os.path.dirname(__file__), "..", "..", "app", "ui", "assets",
                            "background.png")
        width, height, rows = png2bmp.read_png(open(path, "rb").read())
        self.assertEqual((width, height, len(rows)), (1440, 480, 480))


if __name__ == "__main__":
    unittest.main()
