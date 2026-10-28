"""Tests for xbe_title_image.py: DXT1 read back by a decoder here, the XPR0 header, and a section
added to a small XBE made here, with every other byte where it was."""

import hashlib
import os
import struct
import sys
import unittest
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xbe_title_image as t  # noqa: E402

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
BASE = 0x10000


def decode_dxt1(width, height, data):
    """Rows of RGB tuples from four-colour DXT1 blocks (what a dashboard would see)."""
    rows = [[None] * width for _ in range(height)]
    i = 0
    for by in range(0, height, 4):
        for bx in range(0, width, 4):
            c0, c1, bits = struct.unpack_from("<HHI", data, i)
            i += 8
            pal = t.palette(c0, c1) if c0 > c1 else [t.expand565(c0)] * 4
            for p in range(16):
                rows[by + p // 4][bx + p % 4] = pal[(bits >> (2 * p)) & 3]
    return rows


def make_xbe(first_raw=0x1000):
    """An XBE with two sections, .text and .data, laid out as cxbe does."""
    headers = bytearray(0x400)
    headers[:4] = b"XBEH"
    sec_at = 0x178 + 0x60  # after the image header and a (short) certificate
    names_at = sec_at + 2 * t.SECTION_HEADER.size
    pages_at = names_at + 12
    struct.pack_into("<3I", headers, 0x104, BASE, pages_at + 6, 0x3000 + 0x800)
    struct.pack_into("<I", headers, 0x118, BASE + 0x178)
    struct.pack_into("<2I", headers, 0x11C, 2, BASE + sec_at)
    for i, (name, vaddr, raw, size) in enumerate(((b".text", 0x1000, first_raw, 0x1000),
                                                  (b".data", 0x2000, first_raw + 0x1000, 0x1800))):
        t.SECTION_HEADER.pack_into(headers, sec_at + i * t.SECTION_HEADER.size, 2, BASE + vaddr,
                                   size, raw, size, BASE + names_at + 6 * i, 0,
                                   BASE + pages_at + 2 * i, BASE + pages_at + 2 * i + 2, bytes(20))
        headers[names_at + 6 * i:names_at + 6 * i + len(name)] = name
    headers = bytes(headers[:pages_at + 6])
    body = bytearray(first_raw + 0x2000)
    body[:len(headers)] = headers
    body[first_raw:first_raw + 0x1000] = b"T" * 0x1000
    body[first_raw + 0x1000:first_raw + 0x2000] = b"D" * 0x1000
    return bytes(body)


def sections(xbe):
    base, = struct.unpack_from("<I", xbe, 0x104)
    count, at = struct.unpack_from("<2I", xbe, 0x11C)
    out = []
    for i in range(count):
        f = t.SECTION_HEADER.unpack_from(xbe, at - base + i * t.SECTION_HEADER.size)
        name = xbe[f[5] - base:xbe.index(b"\0", f[5] - base)]
        out.append((name, f))
    return out


class Dxt1(unittest.TestCase):
    def test_solid_and_two_colour_blocks_are_exact(self):
        solid = [bytes([248, 252, 248] * 4)] * 4  # exactly representable in RGB565
        data = t.encode_dxt1(4, 4, solid)
        self.assertEqual(len(data), 8)
        self.assertTrue(all(p == (255, 255, 255) for row in decode_dxt1(4, 4, data) for p in row))
        two = [bytes([0, 0, 0] * 2 + [248, 0, 0] * 2)] * 4
        rows = decode_dxt1(4, 4, t.encode_dxt1(4, 4, two))
        self.assertEqual(rows[0][0], (0, 0, 0))
        self.assertEqual(rows[3][3], (255, 0, 0))
        c0, c1, _ = struct.unpack("<HHI", t.encode_dxt1(4, 4, two))
        self.assertGreater(c0, c1)  # four-colour mode: no transparent index

    def test_gradient_stays_close(self):
        # Along one direction, as a block's four colours are (DXT1 can't follow two at once).
        rows = [bytes(v for x in range(16) for v in (x * 16, x * 8, 128)) for y in range(16)]
        out = decode_dxt1(16, 16, t.encode_dxt1(16, 16, rows))
        worst = max(abs(out[y][x][c] - rows[y][3 * x + c]) for y in range(16) for x in range(16)
                    for c in range(3))
        self.assertLessEqual(worst, 12)

    def test_sides_must_be_multiples_of_four(self):
        with self.assertRaises(ValueError):
            t.encode_dxt1(6, 4, [bytes(18)] * 4)


class Xpr(unittest.TestCase):
    def test_header(self):
        dxt = bytes(range(256)) * 32  # 8 KiB, as 128 x 128 DXT1 is
        xpr = t.xpr_texture(dxt)
        magic, total, header = struct.unpack_from("<4sII", xpr)
        self.assertEqual((magic, total, header), (b"XPR0", 0x800 + 8192, 0x800))
        common, data, lock, fmt, size, eoh = struct.unpack_from("<6I", xpr, 12)
        self.assertEqual((common, data, lock, size, eoh), (0x00040001, 0, 0, 0, 0xFFFFFFFF))
        self.assertEqual((fmt >> 4) & 0xF, 2)        # 2D
        self.assertEqual((fmt >> 8) & 0xFF, 0x0C)    # DXT1
        self.assertEqual((fmt >> 16) & 0xF, 1)       # one mipmap level
        self.assertEqual(1 << ((fmt >> 20) & 0xF), 128)
        self.assertEqual(1 << ((fmt >> 24) & 0xF), 128)
        self.assertEqual(xpr[0x800:], dxt)
        self.assertEqual(len(xpr), total)


class AddSection(unittest.TestCase):
    def test_adds_one_section_and_keeps_the_rest(self):
        xbe = make_xbe()
        data = b"\x5A" * 10240
        out = t.add_section(xbe, b"$$XTIMAGE", data, t.FLAG_INSERTED_FILE)
        secs = sections(out)
        self.assertEqual([n for n, _ in secs], [b".text", b".data", b"$$XTIMAGE"])
        self.assertEqual(secs[:2], sections(xbe))  # the old headers, unchanged
        f = secs[2][1]
        flags, vaddr, vsize, raw, rsize = f[:5]
        self.assertEqual(flags, t.FLAG_INSERTED_FILE)
        self.assertEqual(vaddr, BASE + 0x4000)       # the page after .data (0x2000 + 0x1800)
        self.assertEqual((vsize, rsize), (len(data), len(data)))
        self.assertEqual(raw, 0x3000)                # the file's end, a page boundary
        self.assertEqual(out[raw:raw + rsize], data)
        self.assertEqual(f[9], hashlib.sha1(struct.pack("<I", len(data)) + data).digest())
        self.assertNotIn(f[7], [s[1][7] for s in secs[:2]] + [s[1][8] for s in secs[:2]])
        self.assertEqual(f[8], f[7] + 2)
        _base, headers_size, image_size = struct.unpack_from("<3I", out, 0x104)
        self.assertLessEqual(headers_size, 0x1000)   # still before the first section
        self.assertEqual(image_size, vaddr + vsize - BASE)
        self.assertEqual(len(out) % 0x1000, 0)
        # Everything that was there stays, but the image header's three changed fields.
        self.assertEqual(out[0x1000:0x3000], xbe[0x1000:0x3000])
        old_size = struct.unpack_from("<I", xbe, 0x108)[0]
        same = bytearray(out[:old_size])
        same[0x108:0x110] = xbe[0x108:0x110]
        same[0x11C:0x124] = xbe[0x11C:0x124]
        self.assertEqual(bytes(same), xbe[:old_size])

    def test_refuses(self):
        xbe = make_xbe()
        once = t.add_section(xbe, b"$$XTIMAGE", b"x", t.FLAG_INSERTED_FILE)
        with self.assertRaisesRegex(ValueError, "already has"):
            t.add_section(once, b"$$XTIMAGE", b"x", t.FLAG_INSERTED_FILE)
        with self.assertRaisesRegex(ValueError, "no room"):
            t.add_section(make_xbe(first_raw=0x300), b"$$XTIMAGE", b"x", t.FLAG_INSERTED_FILE)
        with self.assertRaisesRegex(ValueError, "not an XBE"):
            t.add_section(b"MZ" + bytes(0x200), b"$$XTIMAGE", b"x", t.FLAG_INSERTED_FILE)


class TitleImage(unittest.TestCase):
    def test_the_apps_image(self):
        with open(os.path.join(ROOT, "app", "assets", "title-image.png"), "rb") as f:
            data = t.title_image(f.read())
        self.assertEqual(len(data), 0x800 + 128 * 128 // 2)
        self.assertEqual(data[:4], b"XPR0")

    def test_wrong_size(self):
        def chunk(kind, body):
            return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
        png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 4, 4, 8, 2, 0, 0, 0))
               + chunk(b"IDAT", zlib.compress((b"\0" + bytes(12)) * 4)) + chunk(b"IEND", b""))
        with self.assertRaisesRegex(ValueError, "128 x 128"):
            t.title_image(png)


if __name__ == "__main__":
    unittest.main()
