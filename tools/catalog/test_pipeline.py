"""Tests for the catalog pipeline on a synthetic cache: placeholder games, written in the
formats of the real sources (Redump's datfile, list and disc pages; Wikidata's SPARQL
JSON). No network, no real titles.

    python3 -I tools/catalog/test_pipeline.py [--cli <xcat-cli>] [--work <dir>]

With --cli, the built catalog is also opened by xcat-cli (the C reader of xcat/catalog.h),
which checks it against catalog.md like the console does.
"""
from __future__ import annotations

import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import build  # noqa: E402
import catalog_bin as cb  # noqa: E402

CLI = None
WORK = None

# (disc number or None, datfile name, category, serial)
DISCS = [
    (101, "Alpha Racer (USA)", "Games", "AA-001"),
    (102, "Alpha Racer (Europe) (En,Fr,De)", "Games", "AA-001"),
    (103, "Alpha Racer (USA) (Rev 1)", "Games", "AA-001, AA-001v2"),
    (104, "Bravo, The - Tactics (Japan)", "Games", "BB-010"),
    (105, "Charlie Quest (USA, Europe)", "Games", "CC-65535"),
    (106, "Delta (Europe) (En,Fr)", "Games", "DD-002"),
    (107, "Delta (Europe) (En,Es)", "Games", "DD-002"),
    (108, "Echo (USA)", "Games", "N/A"),
    (109, "Foxtrot Demo (USA)", "Demos", "FF-001"),
    (110, "Golf (Scandinavia)", "Games", "GG-003"),
    (None, "Hotel (USA)", "Games", ""),  # in the datfile, no disc page
    (112, "India Quest (Germany)", "Games", "II-005"),  # one game, two regional titles
    (113, "India Adventure (USA)", "Games", "II-005"),
    (114, "India Extra (Japan)", "Games", "II-005"),  # [game] = "none" in test_overrides
]
MISSING_PAGE = 111  # listed, but its page isn't in the cache

WIKIDATA = {
    "games": [(1001, "Alpha Racer"), (1002, "The Bravo: Tactics"), (1003, "Unused Game")],
    "developers": [(1001, "Studio Two"), (1001, "Studio One")],
    "publishers": [(1001, "Pub A")],
    "genres": [(1001, "racing"), (1002, "strategy")],
    "dates": [(1001, "2002-03-01T00:00:00Z"), (1001, "2001-11-15T00:00:00Z"), (1002, "1890-01-01T00:00:00Z")],
    "redump": [(1001, "101")],
}


def slurp(path: str) -> bytes:
    with open(path, "rb") as f:
        return f.read()


def sha1_of(name: str) -> bytes:
    import hashlib
    return hashlib.sha1(name.encode()).digest()


def disc_page(number: int, name: str, serial: str) -> str:
    region = name.split(" (")[1].split(")")[0]
    flags = "".join(f'<img src="/images/regions/X.png" alt="{r}" title="{r}" />' for r in region.split(", "))
    sha1 = sha1_of(name).hex()
    return (
        f'<html><h1>{name}</h1><div class="game"><table class="gameinfo" cellspacing="0">\n'
        '<tr><th>System</th><td><a href="/discs/system/xbox/">Microsoft Xbox</a></td></tr>\n'
        '<tr><th>Media</th><td>DVD-9</td></tr>\n'
        f'<tr><th>Region</th><td><a href="/discs/region/X/">{flags}</a></td></tr>\n'
        f'<tr><th>Serial</th><td>{serial}</td></tr><tr><th>Edition</th><td>Original</td></tr>'
        '</table></div><div><table class="tracks" cellspacing="0">'
        '<tr><th>#</th><th>Sectors</th><th>Size</th><th>CRC-32</th><th>MD5</th><th>SHA-1</th></tr>'
        f'<tr><td>1</td><td>3820880</td><td>7825162240</td><td>3442acbb</td>'
        f'<td>{"ab" * 16}</td><td>{sha1}</td></tr></table></div></html>')


def datfile() -> bytes:
    games = "".join(
        f'\t<game name="{n}">\n\t\t<category>{c}</category>\n\t\t<description>{n}</description>\n'
        f'\t\t<rom name="{n}.iso" size="7825162240" crc="0000{i:04x}" md5="{"cd" * 16}" '
        f'sha1="{sha1_of(n).hex()}"/>\n\t</game>\n'
        for i, (_, n, c, _) in enumerate(DISCS))
    games += ('\t<game name="Some App (USA)">\n\t\t<category>Games</category>\n'
              '\t\t<rom name="Some App (USA).cue" size="1" crc="00000000"/>\n'
              '\t\t<rom name="Some App (USA).bin" size="2" crc="00000000"/>\n\t</game>\n')
    xml = ('<?xml version="1.0"?>\n<datafile>\n\t<header>\n\t\t<name>Microsoft - Xbox</name>\n'
           '\t\t<version>2026-06-14 23-43-27</version>\n\t</header>\n' + games + '</datafile>\n')
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        z.writestr(zipfile.ZipInfo("Microsoft - Xbox - Datfile (11).dat", (2026, 6, 14, 0, 0, 0)), xml)
    return buf.getvalue()


def make_cache(root: str) -> None:
    def put(rel: str, data: bytes | str) -> None:
        path = os.path.join(root, *rel.split("/"))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data.encode() if isinstance(data, str) else data)

    put("meta.json", json.dumps({"wikidata": "2026-10-20", "redump": "2026-10-21",
                                 "redump_datfile": "2026-06-14 23-43-27"}))
    put("redump/datfile.zip", datfile())
    numbers = [d[0] for d in DISCS if d[0]] + [MISSING_PAGE]
    pages = [numbers[:6], numbers[6:]]
    for p, ids in enumerate(pages, 1):
        links = "".join(f'<tr><td><a href="/disc/{i}/">x</a></td></tr>' for i in ids)
        put(f"redump/list/{p}.html", f'<div class="pages"><a href="/discs/system/xbox/?page=2">2</a></div>'
                                     f'<table>{links}</table>')
    for number, name, _, serial in DISCS:
        if number:
            put(f"redump/disc/{number}.html", disc_page(number, name, serial))
    for query, rows in WIKIDATA.items():
        bindings = [{"item": {"type": "uri", "value": f"http://www.wikidata.org/entity/Q{q}"},
                     "label": {"type": "literal", "value": v}} for q, v in rows]
        bindings.append({"item": {"value": "not-an-item"}, "label": {"value": "x"}})
        put(f"wikidata/{query}.json", json.dumps({"results": {"bindings": bindings}}))


OVERRIDES = {"region": {"Scandinavia": "SC"}, "title": {"Q1002": "Bravo Tactics"}}


def run_build(cache: str, overrides=OVERRIDES) -> tuple[bytes, build.Report]:
    report = build.Report()
    data, _ = build.build(build.Inputs(cache), overrides, report)
    return data, report


class Pipeline(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp(dir=WORK)
        self.cache = os.path.join(self.tmp, "cache")
        make_cache(self.cache)

    def tearDown(self) -> None:
        shutil.rmtree(self.tmp)

    def test_catalog_contents(self) -> None:
        data, report = run_build(self.cache)
        games, releases = cb.read(data)
        by_name = {r.name: r for r in releases}
        self.assertEqual(sorted(by_name), sorted([
            "Alpha Racer (USA)", "Alpha Racer (Europe) (En,Fr,De)", "Alpha Racer (USA) (Rev 1)",
            "Bravo, The - Tactics (Japan)", "Charlie Quest (USA, Europe)",
            "Delta (Europe) (En,Fr)", "Delta (Europe) (En,Es)", "Golf (Scandinavia)",
            "India Quest (Germany)", "India Adventure (USA)", "India Extra (Japan)"]))
        # Title IDs from serials; variants by region, then tags, then disc number.
        self.assertEqual(by_name["Alpha Racer (USA)"].title_id, 0x41410001)
        self.assertEqual(by_name["Charlie Quest (USA, Europe)"].title_id, 0x4343FFFF)
        self.assertEqual(by_name["Alpha Racer (USA)"].variant, "US")
        self.assertEqual(by_name["Alpha Racer (USA) (Rev 1)"].variant, "US-REV1")
        self.assertEqual(by_name["Alpha Racer (Europe) (En,Fr,De)"].variant, "EU")
        self.assertEqual(by_name["Charlie Quest (USA, Europe)"].variant, "US-EU")
        self.assertEqual(by_name["Delta (Europe) (En,Fr)"].variant, "EU")
        self.assertEqual(by_name["Delta (Europe) (En,Es)"].variant, "EU-2")
        self.assertEqual(by_name["Golf (Scandinavia)"].variant, "SC")
        self.assertEqual(by_name["Alpha Racer (USA)"].redump_id, 101)
        self.assertEqual(by_name["Alpha Racer (USA)"].sha1, sha1_of("Alpha Racer (USA)"))
        self.assertEqual(by_name["Alpha Racer (USA)"].region, "USA")
        # Games: by Redump ID (Alpha), shared title ID (Alpha Europe, Rev 1), name (Bravo),
        # and games of their own; Delta's two releases are one game.
        titles = {g.title: g for g in games}
        self.assertEqual(sorted(titles), ["Alpha Racer", "Bravo Tactics", "Charlie Quest", "Delta", "Golf",
                                          "India Adventure"])
        alpha = titles["Alpha Racer"]
        self.assertEqual((alpha.wikidata, alpha.year, alpha.developer, alpha.publisher, alpha.genres),
                         (1001, 2001, "Studio One, Studio Two", "Pub A", "racing"))
        self.assertEqual({games[r.game].title for r in releases if r.name.startswith("Alpha")}, {"Alpha Racer"})
        self.assertEqual((titles["Bravo Tactics"].wikidata, titles["Bravo Tactics"].year), (1002, 0))
        self.assertEqual(titles["Charlie Quest"].wikidata, 0)
        self.assertEqual(len({r.game for r in releases if r.name.startswith("Delta")}), 1)
        # Without Wikidata data, one title ID is one game, titled by its USA release.
        self.assertEqual({games[r.game].title for r in releases if r.name.startswith("India")},
                         {"India Adventure"})
        # The report says what was left out and what to check.
        text = report.text()
        for line in ("Echo (USA) (serial 'N/A')", "Hotel (USA) (no disc page)", str(MISSING_PAGE),
                     "Bravo, The - Tactics (Japan) -> Q1002", "Alpha Racer (Europe) (En,Fr,De) -> Q1001"):
            self.assertIn(line, text)
        self.assertNotIn("Foxtrot", text + "".join(by_name))
        self.assertIn("India Adventure <- India Adventure | India Extra | India Quest", text)

    def test_deterministic(self) -> None:
        a, _ = run_build(self.cache)
        moved = os.path.join(self.tmp, "elsewhere", "cache")
        shutil.copytree(self.cache, moved)
        b, _ = run_build(moved)
        self.assertEqual(a, b)

    def test_overrides(self) -> None:
        ov = {"region": {"Scandinavia": "SC"}, "exclude": ["Golf (Scandinavia)"],
              "categories": ["Games", "Demos"], "title_id": {"Echo (USA)": "4D530004"},
              "game": {"Charlie Quest (USA, Europe)": "Q1003", "Alpha Racer (Europe) (En,Fr,De)": "none",
                       "India Extra (Japan)": "none"},
              "variant": {"Delta (Europe) (En,Es)": "EU-SPAIN"}, "title": {"Q1003": "Charlie (Renamed)"}}
        data, _ = run_build(self.cache, ov)
        games, releases = cb.read(data)
        by_name = {r.name: r for r in releases}
        self.assertNotIn("Golf (Scandinavia)", by_name)
        self.assertIn("Foxtrot Demo (USA)", by_name)
        self.assertEqual(by_name["Echo (USA)"].title_id, 0x4D530004)
        self.assertEqual(by_name["Delta (Europe) (En,Es)"].variant, "EU-SPAIN")
        self.assertEqual(games[by_name["Charlie Quest (USA, Europe)"].game].title, "Charlie (Renamed)")
        self.assertEqual(games[by_name["Alpha Racer (Europe) (En,Fr,De)"].game].wikidata, 0)
        self.assertEqual(games[by_name["India Extra (Japan)"].game].title, "India Extra")
        self.assertEqual(games[by_name["India Quest (Germany)"].game].title, "India Adventure")

    def test_unknown_region_fails(self) -> None:
        with self.assertRaises(SystemExit) as e:
            run_build(self.cache, {})
        self.assertIn("Scandinavia", str(e.exception))

    def test_lock(self) -> None:
        out = os.path.join(self.tmp, "out")
        lock = os.path.join(self.tmp, "inputs.lock")
        ov = os.path.join(self.tmp, "overrides.toml")
        with open(ov, "w", encoding="utf-8") as f:
            f.write('[region]\nScandinavia = "SC"\n')
        cmd = [sys.executable, "-I", os.path.join(HERE, "build.py"), "--cache", self.cache,
               "--out", out, "--lock", lock, "--overrides", ov]
        run = lambda *extra: subprocess.run(cmd + list(extra), capture_output=True, text=True)  # noqa: E731
        self.assertNotEqual(run().returncode, 0)  # no lock yet
        self.assertEqual(run("--update-lock").returncode, 0, run("--update-lock").stderr)
        first = slurp(os.path.join(out, "catalog.bin"))
        self.assertEqual(run().returncode, 0)
        credits = slurp(os.path.join(out, "CREDITS.txt"))
        self.assertIn(b"Snapshot: wikidata 2026-10-20; redump 2026-10-21", credits)
        # The FreeType License's credit (ADR 0015, Consequences).
        self.assertIn(b"Portions of this software are copyright (C) 1996-2013 The FreeType Project", credits)
        r = run("--previous", os.path.join(out, "catalog.bin"))
        self.assertEqual(r.returncode, 0, r.stderr)
        # A changed input is a new snapshot, named by its group.
        with open(os.path.join(self.cache, "redump", "disc", "105.html"), "a") as f:
            f.write(" ")
        r = run()
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("redump-disc", r.stderr)
        # So is a changed catalog from the same inputs (code or overrides).
        self.assertEqual(run("--update-lock").returncode, 0)
        with open(ov, "a", encoding="utf-8") as f:
            f.write('[variant]\n"Delta (Europe) (En,Es)" = "EU-ES"\n')
        r = run()
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("different catalog.bin", r.stderr)
        with open(os.path.join(self.tmp, "first.bin"), "wb") as f:
            f.write(first)
        r = run("--update-lock", "--previous", os.path.join(self.tmp, "first.bin"))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn(b"Delta (Europe) (En,Es): 44440002@EU-2 -> @EU-ES", slurp(os.path.join(out, "report.txt")))

    def test_c_reader_accepts_it(self) -> None:
        if not os.environ.get("XCAT_CLI"):  # set by main() from --cli, after class creation
            self.skipTest("needs --cli")
        data, _ = run_build(self.cache)
        path = os.path.join(self.tmp, "catalog.bin")
        with open(path, "wb") as f:
            f.write(data)
        r = subprocess.run([os.environ["XCAT_CLI"], "catalog", path, "--name", "delta (europe) (en,es)"],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("44440002@EU-2", r.stdout)
        self.assertIn("11 releases", r.stdout)


def main() -> None:
    global WORK
    args = sys.argv[1:]
    if "--cli" in args:
        i = args.index("--cli")
        os.environ["XCAT_CLI"] = args[i + 1]
        del args[i:i + 2]
    if "--work" in args:
        i = args.index("--work")
        WORK = args[i + 1]
        os.makedirs(WORK, exist_ok=True)
        del args[i:i + 2]
    unittest.main(argv=[sys.argv[0]] + args, verbosity=2)


if __name__ == "__main__":
    main()
