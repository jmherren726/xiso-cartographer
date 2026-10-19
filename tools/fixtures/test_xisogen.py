"""Unit tests for the fixture generator (run: python -m unittest discover -s tools/fixtures).

The end-to-end check is extract-xiso (tools/fixtures/verify.sh); these cover
the generator's own rules and input validation.
"""

from __future__ import annotations

import hashlib
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import xisogen as xg  # noqa: E402


def pat(name: str, size: int) -> xg.Node:
    return xg.file(name, size, xg.Content("pattern", name))


class ContentTests(unittest.TestCase):
    def test_pattern_ranges_are_consistent(self):
        c = xg.Content("pattern", "seed")
        whole = c.pattern(0, 1000)
        self.assertEqual(c.pattern(10, 100), whole[10:110])
        self.assertEqual(c.pattern(63, 2), whole[63:65])

    def test_pattern_first_block_definition(self):
        c = xg.Content("pattern", "seed")
        self.assertEqual(c.pattern(0, 64), hashlib.blake2b(b"seed:0", digest_size=64).digest())

    def test_sha256_includes_holes(self):
        c = xg.Content("sparse", "s", edge=4)
        data = bytearray(20)
        data[:4] = c.pattern(0, 4)
        data[16:] = c.pattern(16, 4)
        self.assertEqual(c.sha256(20), hashlib.sha256(bytes(data)).hexdigest())

    def test_sparse_small_file_is_all_pattern(self):
        c = xg.Content("sparse", "s", edge=4)
        self.assertEqual(list(c.regions(6)), [(0, 6)])


class NameTests(unittest.TestCase):
    def test_sort_key_folds_only_ascii_letters(self):
        self.assertEqual(xg.sort_key("aZ_~"), b"AZ_~")
        self.assertLess(xg.sort_key("B"), xg.sort_key("_"))  # 'B' < '_' after folding

    def test_invalid_names_rejected(self):
        # Non-ASCII is rejected too: UnicodeEncodeError is a ValueError.
        for bad in ["", ".", "..", "a/b", "a\\b", "tab\tname", "x" * 256, "café"]:
            with self.subTest(name=bad), self.assertRaises(ValueError):
                xg.check_name(bad)

    def test_case_insensitive_collision_rejected(self):
        with self.assertRaises(ValueError):
            xg.build_table([pat("Readme", 1), pat("README", 1)])


class TableTests(unittest.TestCase):
    def test_empty_directory_is_one_pad_sector(self):
        self.assertEqual(xg.build_table([]), b"\xff" * xg.SECTOR)

    def test_root_entry_at_offset_zero_and_tree_ordered(self):
        children = [pat(n, 1) for n in ["c", "a", "b"]]
        table = xg.build_table(children)
        left, right, _, _, _, nlen = struct.unpack_from("<HHIIBB", table, 0)
        self.assertEqual(table[14 : 14 + nlen], b"b")  # middle of a, b, c
        self.assertEqual(table[left * 4 + 14 : left * 4 + 15], b"a")
        self.assertEqual(table[right * 4 + 14 : right * 4 + 15], b"c")

    def test_entries_never_cross_sectors(self):
        children = [pat(f"{i:03d}" + "n" * 60, 1) for i in range(100)]
        table = xg.build_table(children)
        self.assertGreater(len(table), xg.SECTOR)
        # Walk the tree and check every entry stays inside one sector.
        stack = [0]
        seen = 0
        while stack:
            off = stack.pop()
            left, right, _, _, _, nlen = struct.unpack_from("<HHIIBB", table, off)
            end = off + 14 + nlen - 1
            self.assertEqual(off // xg.SECTOR, end // xg.SECTOR)
            seen += 1
            stack += [o * 4 for o in (left, right) if o]
        self.assertEqual(seen, 100)
        self.assertEqual(len(table) % xg.SECTOR, 0)


class ImageTests(unittest.TestCase):
    def test_file_size_must_fit_32_bits(self):
        img = xg.Image("t", "xiso", [xg.file("big", 1 << 32, xg.Content("sparse", "x"))])
        with self.assertRaises(ValueError):
            xg.layout(img)

    def test_volume_descriptor_and_manifest(self):
        img = xg.Image("t", "xiso", [pat("a.bin", 3000), xg.directory("d", [pat("b.bin", 5)])])
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "t.iso")
            xg.write(img, path)
            with open(path, "rb") as f:
                f.seek(xg.VOLUME_DESCRIPTOR_OFFSET)
                vd = f.read(xg.SECTOR)
            size = os.path.getsize(path)
        self.assertEqual(vd[:20], xg.MAGIC)
        self.assertEqual(vd[-20:], xg.MAGIC)
        root_sector, root_size = struct.unpack_from("<II", vd, 20)
        self.assertEqual(root_sector, xg.ROOT_TABLE_SECTOR)
        self.assertEqual(root_size, xg.SECTOR)
        self.assertEqual(size, img.size)
        self.assertEqual(size % xg.IMAGE_MODULUS, 0)
        m = xg.manifest(img, None)
        offsets = [e["offset"] for e in m["files"]]
        self.assertEqual(offsets, sorted(offsets))
        self.assertEqual({e["path"] for e in m["files"]}, {"a.bin", "d/b.bin"})

    def test_full_disc_layout_offsets_and_size(self):
        img = xg.Image("t", "xgd3", [pat("a.bin", 10)])
        xg.layout(img)
        self.assertEqual(img.size, xg.FULL_DISC_SIZE["xgd3"])
        e = img.entries[0]
        self.assertEqual(e["offset"], xg.LAYOUTS["xgd3"] + e["sector"] * xg.SECTOR)

    def _ordered(self, order, gap=0):
        tree = [pat("a.bin", 3000), xg.directory("d", [pat("b.bin", 5), xg.directory("e", [pat("c.bin", 7)])])]
        img = xg.Image("t", "xiso", tree, order=order, gap_sectors=gap)
        xg.layout(img)
        files = {e["path"]: e["sector"] for e in img.entries if e["type"] == "file"}
        dirs = {e["path"]: e["sector"] for e in img.entries if e["type"] == "dir"}
        return img, files, dirs

    def test_tables_last_order(self):
        img, files, dirs = self._ordered("tables-last", gap=2)
        self.assertGreater(min(dirs.values()), max(files.values()))  # every table after every file
        self.assertLess(dirs["d/e"], dirs["d"])  # a child's table before its parent's
        self.assertGreater(img.root_sector, max(dirs.values()))  # the root last
        self.assertEqual(files["d/b.bin"] - files["a.bin"], 2 + 2)  # 3000 bytes = 2 sectors, + gap

    def test_root_last_order(self):
        img, files, dirs = self._ordered("root-last")
        self.assertLess(max(dirs.values()), min(files.values()))  # subdirectory tables first
        self.assertLess(dirs["d/e"], dirs["d"])  # deepest first
        self.assertGreater(img.root_sector, max(files.values()))  # the root after every file

    def test_unknown_order(self):
        with self.assertRaises(ValueError):
            xg.layout(xg.Image("t", "xiso", [pat("a.bin", 1)], order="sideways"))


if __name__ == "__main__":
    unittest.main()
