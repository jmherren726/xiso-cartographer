"""Builds catalog.bin, CREDITS.txt and a review report from the cached inputs (ADR 0014).

    python3 -I tools/catalog/build.py --cache <dir> --out <dir> [--update-lock]
                                      [--lock tools/catalog/inputs.lock]
                                      [--overrides tools/catalog/overrides.toml]
                                      [--previous <old catalog.bin>]

Offline and deterministic: no clock, no network, no locale, every order fixed. The cache
must match inputs.lock (the SHA-256 of each group of inputs and of the catalog they
build) unless --update-lock rewrites it. The rules are in README.md.
"""
from __future__ import annotations

import argparse
import collections
import hashlib
import json
import os
import re
import sys

try:
    import tomllib
except ImportError:  # Python < 3.11
    raise SystemExit("build.py needs Python 3.11 or newer (tomllib)")

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import catalog_bin as cb  # noqa: E402
import sources  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))

# README.md "Variant names": Redump regions and their codes.
REGION_CODES = {
    "USA": "US", "Europe": "EU", "Japan": "JP", "Asia": "AS", "Korea": "KR",
    "Australia": "AU", "World": "WW", "Argentina": "AR", "Austria": "AT", "Belgium": "BE",
    "Brazil": "BR", "Canada": "CA", "Chile": "CL", "China": "CN", "Croatia": "HR",
    "Czech": "CZ", "Denmark": "DK", "Finland": "FI", "France": "FR", "Germany": "DE",
    "Greece": "GR", "Hong Kong": "HK", "Hungary": "HU", "India": "IN", "Ireland": "IE",
    "Israel": "IL", "Italy": "IT", "Mexico": "MX", "Netherlands": "NL", "New Zealand": "NZ",
    "Norway": "NO", "Poland": "PL", "Portugal": "PT", "Russia": "RU", "Singapore": "SG",
    "South Africa": "ZA", "Spain": "ES", "Sweden": "SE", "Switzerland": "CH", "Taiwan": "TW",
    "Turkey": "TR", "UK": "GB",
}
# Whose title a game without Wikidata data shows, in this order (README.md "Games").
TITLE_REGIONS = ["USA", "USA, Europe", "World", "Europe", "UK", "Australia"]
SERIAL = re.compile(r"(?<![A-Z])([A-Z]{2})-(\d{1,5})(?![0-9])")
LANGUAGES = re.compile(r"[A-Z][a-z](-[A-Z][a-z]+)?(,[A-Z][a-z](-[A-Z][a-z]+)?)*")
TAG = re.compile(r" \(([^()]*)\)")


# ---- Inputs and the lock ---------------------------------------------------------------

def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class Inputs:
    """The cached files a build reads, by group, each with its SHA-256."""

    def __init__(self, cache: str) -> None:
        self.cache = cache
        self.groups: dict[str, dict[str, str]] = collections.defaultdict(dict)

    def read(self, group: str, rel: str) -> bytes:
        with open(os.path.join(self.cache, *rel.split("/")), "rb") as f:
            data = f.read()
        self.groups[group][rel] = sha256(data)
        return data

    def digests(self) -> dict[str, tuple[int, str]]:
        out = {}
        for group, files in sorted(self.groups.items()):
            listing = "".join(f"{rel}\0{h}\n" for rel, h in sorted(files.items()))
            out[group] = (len(files), sha256(listing.encode()))
        return out


def read_lock(path: str) -> dict[str, str]:
    lock = {}
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            for line in f:
                if line.strip() and not line.startswith("#"):
                    key, value = line.split(None, 1)
                    lock[key] = value.strip()
    return lock


def write_lock(path: str, digests: dict[str, tuple[int, str]], catalog_sha: str) -> None:
    lines = ["# SHA-256 of the catalog's cached inputs, per group (count, then the digest of",
             "# their sorted \"path NUL sha256\" lines), and of the catalog.bin they build.",
             "# Written by tools/catalog/build.py --update-lock (README.md); never edited by hand."]
    lines += [f"{g} {n} {h}" for g, (n, h) in digests.items()]
    lines.append(f"catalog.bin {catalog_sha}")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


# ---- Names -----------------------------------------------------------------------------

def name_parts(name: str) -> tuple[str, str, list[str]]:
    """A Redump name's base title, its regions (first tag) and its other tags (languages
    left out): "Game (USA) (En,Fr) (Rev 1)" -> ("Game", "USA", ["Rev 1"])."""
    base = name.split(" (", 1)[0]
    tags = TAG.findall(name[len(base):])
    region = tags[0] if tags else ""
    extra = [t for t in tags[1:] if not LANGUAGES.fullmatch(t)]
    return base, region, extra


def display_title(base: str) -> str:
    """Redump's "Elder Scrolls III, The - Morrowind" -> "The Elder Scrolls III: Morrowind"."""
    first, sep, rest = base.partition(" - ")
    m = re.fullmatch(r"(.*), (The|A|An)", first)
    if m:
        first = f"{m.group(2)} {m.group(1)}"
    return first + (": " + rest.replace(" - ", ": ") if sep else "")


def match_key(title: str) -> str:
    """For matching Redump titles to Wikidata labels: letters and digits, lower case."""
    t = title.replace("&", " and ").lower()
    return "".join(c for c in t if c.isalnum())


def title_id_from_serial(serial: str) -> int | None:
    """README.md "Title IDs from serials": the first XX-NNN that fits."""
    for m in SERIAL.finditer(serial):
        number = int(m.group(2))
        if number <= 0xFFFF:
            return (ord(m.group(1)[0]) << 24) | (ord(m.group(1)[1]) << 16) | number
    return None


def fit(values: list[str]) -> str:
    """Joined with ", " in §6.1 order, dropping the last ones until it fits 255 bytes."""
    values = sorted(set(values), key=cb.collate_key)
    while values and len(", ".join(values).encode()) > cb.MAX_STRING:
        values.pop()
    return ", ".join(values)


# ---- The build -------------------------------------------------------------------------

class Report:
    def __init__(self) -> None:
        self.sections: dict[str, list[str]] = collections.defaultdict(list)
        self.errors: list[str] = []

    def add(self, section: str, line: str) -> None:
        self.sections[section].append(line)

    def text(self) -> str:
        out = []
        for section, lines in sorted(self.sections.items()):
            out.append(f"## {section} ({len(lines)})")
            out += sorted(lines, key=cb.collate_key)
            out.append("")
        if self.errors:
            out.append(f"## ERRORS ({len(self.errors)})")
            out += self.errors
        return "\n".join(out) + "\n"


def variants(releases: list[dict], overrides: dict, report: Report) -> None:
    """README.md "Variant names", steps 1-4, then overrides. Sets r["variant"]."""
    for r in releases:
        codes = []
        for region in r["region"].split(", "):
            code = overrides.get("region", {}).get(region, REGION_CODES.get(region))
            if code is None:
                report.errors.append(f"unknown region {region!r} in {r['name']!r}: add it to [region]")
                code = "XX"
            codes.append(code)
        r["variant"] = "-".join(codes)[:cb.MAX_VARIANT]
    by_key = collections.defaultdict(list)
    for r in releases:
        by_key[(r["title_id"], r["variant"])].append(r)
    for group in by_key.values():
        if len(group) > 1:  # step 2: the tags that set them apart
            for r in group:
                suffix = "-".join("".join(c for c in t.upper() if c.isascii() and c.isalnum())
                                  for t in r["extra"])
                if suffix:
                    r["variant"] = (r["variant"] + "-" + suffix)[:cb.MAX_VARIANT]
    by_key = collections.defaultdict(list)
    for r in releases:
        by_key[(r["title_id"], r["variant"])].append(r)
    for group in by_key.values():  # step 3, with step 4's cut
        group.sort(key=lambda r: (r["redump_id"], cb.collate_key(r["name"])))
        for n, r in enumerate(group[1:], 2):
            sfx = f"-{n}"
            r["variant"] = r["variant"][:cb.MAX_VARIANT - len(sfx)] + sfx
    for r in releases:
        forced = overrides.get("variant", {}).get(r["name"])
        if forced is not None:
            r["variant"] = forced


def build(inputs: Inputs, overrides: dict, report: Report) -> tuple[bytes, dict]:
    meta = json.loads(inputs.read("meta", "meta.json"))
    version, dat = sources.read_datfile(inputs.read("redump-datfile", "redump/datfile.zip"))

    # Disc pages of every disc the list pages name.
    list_dir = os.path.join(inputs.cache, "redump", "list")
    disc_ids: set[int] = set()
    for page in sorted(int(f[:-5]) for f in os.listdir(list_dir) if re.fullmatch(r"\d+\.html", f)):
        ids, _ = sources.read_list_page(inputs.read("redump-list", f"redump/list/{page}.html").decode())
        disc_ids.update(ids)
    by_sha1: dict[bytes, sources.Disc] = {}
    for i in sorted(disc_ids):
        rel = f"redump/disc/{i}.html"
        if not os.path.exists(os.path.join(inputs.cache, *rel.split("/"))):
            report.add("Disc pages missing from the cache (run fetch.py)", str(i))
            continue
        disc = sources.read_disc_page(i, inputs.read("redump-disc", rel).decode("utf-8"))
        if disc.system != "Microsoft Xbox" or not disc.sha1:
            report.add("Disc pages without one Xbox track", str(i))
        elif disc.sha1 in by_sha1:
            report.add("Disc pages with the same SHA-1 (lowest number kept)", str(i))
        else:
            by_sha1[disc.sha1] = disc

    wd = {name: sources.read_wikidata(name, inputs.read("wikidata", f"wikidata/{name}.json").decode())
          for name in sorted(sources.WIKIDATA_QUERIES)}
    labels = {}
    for q, label in wd["games"]:
        labels.setdefault(q, label)  # sorted, so the first label in byte order
    facts = {k: collections.defaultdict(list) for k in ("developers", "publishers", "genres")}
    for k in facts:
        for q, v in wd[k]:
            facts[k][q].append(v)
    years: dict[int, int] = {}
    for q, v in wd["dates"]:
        if re.match(r"\d{4}-", v) and 1990 <= int(v[:4]) <= 2100:
            years[q] = min(years.get(q, 9999), int(v[:4]))
    redump_q: dict[int, int] = {}
    for q, v in wd["redump"]:
        if v.isdigit() and q in labels:
            redump_q.setdefault(int(v), q)  # sorted by Q, so the lowest Q wins
    by_title = collections.defaultdict(set)
    for q, label in labels.items():
        by_title[match_key(label)].add(q)

    # Releases: datfile entries in the chosen categories, tied to their disc page.
    categories = set(overrides.get("categories", ["Games"]))
    exclude = set(overrides.get("exclude", []))
    releases = []
    for e in sorted(dat, key=lambda e: cb.collate_key(e.name)):
        if e.category not in categories or e.name in exclude:
            continue
        disc = by_sha1.get(e.sha1)
        forced_id = overrides.get("title_id", {}).get(e.name)
        title_id = int(forced_id, 16) if forced_id else (title_id_from_serial(disc.serial) if disc else None)
        if title_id is None:
            why = f"serial {disc.serial!r}" if disc else "no disc page"
            report.add("Left out: no title ID (set one in [title_id])", f"{e.name} ({why})")
            continue
        base, region, extra = name_parts(e.name)
        releases.append({"name": e.name, "base": base, "region": region, "extra": extra,
                         "title_id": title_id, "redump_id": disc.redump_id if disc else 0,
                         "entry": e, "q": None})

    # Games (README.md "Joining"): overrides, Redump IDs, unique names, shared title IDs.
    for r in releases:
        forced = overrides.get("game", {}).get(r["name"])
        if forced is not None:
            r["q"] = int(forced[1:]) if forced.startswith("Q") else 0
            if r["q"] and r["q"] not in labels:
                report.errors.append(f"[game] {r['name']!r} = {forced}: not an Xbox game in Wikidata")
                r["q"] = 0
            report.add("Joined by overrides.toml", r["name"])
        elif r["redump_id"] in redump_q:
            r["q"] = redump_q[r["redump_id"]]
        else:
            qs = by_title.get(match_key(display_title(r["base"])), set())
            if len(qs) == 1:
                r["q"] = next(iter(qs))
                report.add("Joined by name (check these)", f"{r['name']} -> Q{r['q']} {labels[r['q']]}")
    by_id = collections.defaultdict(set)
    for r in releases:
        if r["q"]:
            by_id[r["title_id"]].add(r["q"])
    for r in releases:
        if r["q"] is None and len(by_id.get(r["title_id"], ())) == 1:
            r["q"] = next(iter(by_id[r["title_id"]]))
            report.add("Joined by a shared title ID (check these)", f"{r['name']} -> Q{r['q']} {labels[r['q']]}")
        elif r["q"] is None:
            report.add("Games without Wikidata data", r["name"])

    # Step 5: releases without Wikidata data share a game by title ID (one game sold under
    # several regional names), except "none" overrides, which group by their own title.
    groups: dict[object, list[dict]] = {}
    for r in releases:
        if r["q"]:
            key: object = r["q"]
        elif overrides.get("game", {}).get(r["name"]) == "none":
            key = ("none", match_key(display_title(r["base"])))
        else:
            key = ("title_id", r["title_id"])
        groups.setdefault(key, []).append(r)
    games: list[cb.Game] = []
    titles = overrides.get("title", {})
    for key, group in groups.items():
        for r in group:
            r["game"] = len(games)
        q = group[0]["q"]
        if q:
            games.append(cb.Game(titles.get(f"Q{q}", labels[q]), fit(facts["developers"][q]),
                                 fit(facts["publishers"][q]), fit(facts["genres"][q]), q,
                                 years.get(q, 0)))
            continue
        # Its title: from [title] by any of its releases' names, else the USA release's.
        named = [titles[r["name"]] for r in group if r["name"] in titles]
        first = min(group, key=lambda r: (TITLE_REGIONS.index(r["region"]) if r["region"] in TITLE_REGIONS
                                          else len(TITLE_REGIONS), cb.collate_key(r["name"])))
        games.append(cb.Game(min(named, key=cb.collate_key) if named else display_title(first["base"])))
        if len({match_key(display_title(r["base"])) for r in group}) > 1:
            report.add("Games without Wikidata data, with several titles (check these)",
                       f"{games[-1].title} <- " + " | ".join(sorted({r['base'] for r in group})))

    variants(releases, overrides, report)
    if report.errors:
        raise SystemExit("build.py: errors (see the report):\n  " + "\n  ".join(report.errors))
    snapshot = f"wikidata {meta['wikidata']}; redump {meta['redump']} (datfile {version})"
    out = cb.build(games, [cb.Release(r["game"], r["title_id"], r["variant"], r["name"], r["region"],
                                      r["redump_id"], r["entry"].size, r["entry"].crc32,
                                      r["entry"].md5, r["entry"].sha1) for r in releases], snapshot)
    report.add("Summary", f"{len(games)} games, {len(releases)} releases; snapshot: {snapshot}")
    return out, {"snapshot": snapshot, "meta": meta, "datfile": version}


def credits(info: dict) -> str:
    return f"""xiso-cartographer: credits

CATALOG
Snapshot: {info['snapshot']}

Game data (titles, developers, publishers, genres, years) comes from Wikidata
(https://www.wikidata.org/), available under CC0 1.0. Thank you to its contributors.

Release data (Redump names, regions, serials, disc numbers, image sizes and hashes)
comes from the Redump project (http://redump.org/), whose members dump and verify
original discs. Only these facts are used; Redump's database isn't redistributed.

The catalog's own content (joins and corrections) is released under CC0 1.0.
Details: tools/catalog/SOURCES.md in the xiso-cartographer repository.

SOFTWARE
The app draws its text with FreeType 2.4.12, built into nxdk's SDL_ttf, under the
FreeType License (FTL):
Portions of this software are copyright (C) 1996-2013 The FreeType Project
(www.freetype.org). All rights reserved.

Every other component in the release, and its licence, is listed in THIRD_PARTY.md
in the xiso-cartographer repository.
"""


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cache", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--lock", default=os.path.join(HERE, "inputs.lock"))
    ap.add_argument("--overrides", default=os.path.join(HERE, "overrides.toml"))
    ap.add_argument("--update-lock", action="store_true")
    ap.add_argument("--previous", help="an older catalog.bin: report variants that changed")
    args = ap.parse_args()

    with open(args.overrides, "rb") as f:
        overrides = tomllib.load(f)
    inputs = Inputs(os.path.abspath(args.cache))
    report = Report()
    data, info = build(inputs, overrides, report)
    digests = inputs.digests()
    catalog_sha = sha256(data)

    if args.previous:
        with open(args.previous, "rb") as f:
            old = {(r.title_id, r.name): r.variant for r in cb.read(f.read())[1]}
        for r in cb.read(data)[1]:
            was = old.get((r.title_id, r.name))
            if was is not None and was != r.variant:
                report.add("Variants changed since --previous (breaks map keys)",
                           f"{r.name}: {r.title_id:08X}@{was} -> @{r.variant}")

    os.makedirs(args.out, exist_ok=True)
    for name, content in (("catalog.bin", data), ("CREDITS.txt", credits(info).encode()),
                          ("report.txt", report.text().encode())):
        with open(os.path.join(args.out, name), "wb") as f:
            f.write(content)
    print(report.sections["Summary"][0])
    print(f"catalog.bin sha256 {catalog_sha}")

    if args.update_lock:
        write_lock(args.lock, digests, catalog_sha)
        print(f"wrote {args.lock}")
        return
    lock = read_lock(args.lock)
    bad = [g for g, (n, h) in digests.items() if lock.get(g) != f"{n} {h}"]
    bad += [g for g in lock if g != "catalog.bin" and g not in digests]
    if bad:
        raise SystemExit(f"build.py: the cache doesn't match {args.lock} in: {', '.join(bad)}. "
                         "A new snapshot? Then rerun with --update-lock and review the report.")
    if lock.get("catalog.bin") != catalog_sha:
        raise SystemExit(f"build.py: same inputs, different catalog.bin than {args.lock} records "
                         "(code or overrides changed?). Rerun with --update-lock if intended.")
    print(f"matches {args.lock}")


if __name__ == "__main__":
    main()
