"""Tests for package.py, on a made-up repo and app/bin: what the zip holds, that it's the same
every time, and what it refuses."""

import os
import shutil
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import package  # noqa: E402


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


class Package(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.root = os.path.join(self.tmp, "repo")
        write(os.path.join(self.root, "core/include/xcat/version.h"),
              b"#define XC_VERSION_MAJOR 0\n#define XC_VERSION_MINOR 9\n#define XC_VERSION_PATCH 1\n")
        write(os.path.join(self.root, "LICENSE"), b"MIT License\nline two\n")
        for _dest, _what, src in package.LICENSES:
            write(os.path.join(self.root, src), ("licence of %s\n" % src).encode())
        self.bin = os.path.join(self.tmp, "bin")
        write(os.path.join(self.bin, "default.xbe"), b"XBEH" + bytes(100))
        write(os.path.join(self.bin, "fonts", "NotoSans-Regular.ttf"), b"font")
        write(os.path.join(self.bin, "fonts", "OFL.txt"), b"ofl")
        write(os.path.join(self.bin, "ui", "background.bmp"), b"BM")
        write(os.path.join(self.bin, "samples", "small.iso"), b"test image")
        self.out = os.path.join(self.tmp, "dist")

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def run_package(self, *extra):
        return package.main(["package.py", "--bin", self.bin, "--out", self.out, "--root", self.root]
                            + list(extra))

    def test_contents(self):
        self.assertEqual(self.run_package("--tag", "v0.9.1"), 0)
        path = os.path.join(self.out, "xiso-cartographer-0.9.1.zip")
        with zipfile.ZipFile(path) as z:
            names = z.namelist()
            self.assertEqual(names, sorted(names))
            self.assertTrue(all(n.startswith("xiso-cartographer/") for n in names))
            short = {n[len("xiso-cartographer/"):] for n in names}
            for want in ("default.xbe", "fonts/NotoSans-Regular.ttf", "fonts/OFL.txt",
                         "ui/background.bmp", "README.txt", "LICENSE.txt", "THIRD_PARTY.txt",
                         "licenses/Mozilla-CA.txt"):
                self.assertIn(want, short)
            for dest, _what, _src in package.LICENSES:
                self.assertIn("licenses/" + dest, short)
            self.assertFalse(any(n.startswith("samples/") for n in short))  # the test image stays out
            self.assertEqual(z.read("xiso-cartographer/default.xbe"), b"XBEH" + bytes(100))
            self.assertEqual(z.read("xiso-cartographer/LICENSE.txt"), b"MIT License\r\nline two\r\n")
            readme = z.read("xiso-cartographer/README.txt").decode()
            self.assertIn("xiso-cartographer 0.9.1", readme)
            self.assertNotIn("\n", readme.replace("\r\n", ""))  # CRLF throughout
            third = z.read("xiso-cartographer/THIRD_PARTY.txt").decode()
            self.assertIn("The FreeType Project", third)
            self.assertIn("licenses\\libusbohci.txt", third)
        with open(path + ".sha256") as f:
            self.assertTrue(f.read().endswith("  xiso-cartographer-0.9.1.zip\n"))

    def test_same_bytes_every_time(self):
        os.environ["SOURCE_DATE_EPOCH"] = "1798761600"
        try:
            self.assertEqual(self.run_package(), 0)
            path = os.path.join(self.out, "xiso-cartographer-0.9.1.zip")
            with open(path, "rb") as f:
                first = f.read()
            os.remove(path)
            self.assertEqual(self.run_package(), 0)
            with open(path, "rb") as f:
                self.assertEqual(f.read(), first)
            with zipfile.ZipFile(path) as z:
                self.assertEqual(z.infolist()[0].date_time[:3], (2027, 1, 1))
        finally:
            del os.environ["SOURCE_DATE_EPOCH"]

    def test_refuses(self):
        self.assertEqual(self.run_package("--tag", "v1.0.0"), 1)   # not version.h's version
        os.remove(os.path.join(self.root, package.LICENSES[0][2]))
        self.assertEqual(self.run_package(), 1)                    # a licence is missing
        write(os.path.join(self.root, package.LICENSES[0][2]), b"back")
        os.remove(os.path.join(self.bin, "default.xbe"))
        self.assertEqual(self.run_package(), 1)                    # no XBE built
        self.assertFalse(os.path.exists(os.path.join(self.out, "xiso-cartographer-0.9.1.zip")))


if __name__ == "__main__":
    unittest.main()
