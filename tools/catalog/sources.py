"""Parsers for the cached raw inputs (SOURCES.md): Redump's Xbox datfile, list pages and
disc pages, and Wikidata's SPARQL answers.

Everything here is input from the network, so every parser is strict: what it doesn't
recognise is skipped and counted, never guessed at. Only the factual fields ADR 0014
names are taken.
"""
from __future__ import annotations

import html
import io
import json
import re
import xml.etree.ElementTree as ET
import zipfile
from dataclasses import dataclass

# ---- Redump datfile: names and full-disc hashes -------------------------------------


@dataclass(frozen=True)
class DatEntry:
    name: str  # the Redump name, e.g. "Game (USA) (En,Fr)"
    category: str
    size: int
    crc32: int
    md5: bytes
    sha1: bytes


def read_datfile(zip_bytes: bytes) -> tuple[str, list[DatEntry]]:
    """Returns the datfile's version ("2026-06-14 23-43-27") and its single-.iso games.

    Python's ElementTree doesn't fetch external DTDs or expand external entities, and the
    zip holds exactly one .dat member or it is refused.
    """
    with zipfile.ZipFile(io.BytesIO(zip_bytes)) as z:
        members = [n for n in z.namelist() if n.lower().endswith(".dat")]
        if len(members) != 1 or z.getinfo(members[0]).file_size > 64 * 1024 * 1024:
            raise ValueError("the datfile zip must hold one .dat of at most 64 MiB")
        root = ET.fromstring(z.read(members[0]))
    if root.tag != "datafile":
        raise ValueError("not a Logiqx datafile")
    version = (root.findtext("header/version") or "").strip()
    out = []
    for game in root.iter("game"):
        roms = game.findall("rom")
        if len(roms) != 1 or not roms[0].get("name", "").lower().endswith(".iso"):
            continue  # cue/bin applications, multi-file entries
        r = roms[0]
        try:
            entry = DatEntry(
                name=game.get("name", ""),
                category=(game.findtext("category") or "").strip(),
                size=int(r.get("size", "")),
                crc32=int(r.get("crc", ""), 16),
                md5=bytes.fromhex(r.get("md5", "")),
                sha1=bytes.fromhex(r.get("sha1", "")),
            )
        except ValueError:
            continue
        if entry.name and len(entry.md5) == 16 and len(entry.sha1) == 20 and entry.size > 0:
            out.append(entry)
    return version, out


# ---- Redump list pages: which discs exist --------------------------------------------

_DISC_LINK = re.compile(r'<a href="/disc/(\d{1,9})/">')
_PAGE_LINK = re.compile(r'href="/discs/system/xbox/\?page=(\d{1,4})"')


def read_list_page(text: str) -> tuple[list[int], int]:
    """Returns the disc numbers on a list page and the highest page number it links to."""
    ids = sorted({int(m) for m in _DISC_LINK.findall(text)})
    pages = [int(p) for p in _PAGE_LINK.findall(text)]
    return ids, max(pages, default=1)


# ---- Redump disc pages: serial, region, edition, and the hashes that tie a disc to its
# datfile entry --------------------------------------------------------------------------


@dataclass(frozen=True)
class Disc:
    redump_id: int
    system: str
    serial: str
    region: str  # as the region images' titles say, joined with ", "
    edition: str
    version: str
    sha1: bytes  # of the only track; empty if the page has none or several


_INFO_ROW = re.compile(r"<tr><th>([^<]{1,40})</th><td>(.*?)</td></tr>", re.S)
_IMG_TITLE = re.compile(r'<img [^>]*title="([^"]{1,60})"')
_TRACK_ROW = re.compile(
    r"<tr><td>(\d+)</td><td>\d+</td><td>\d+</td><td>[0-9a-f]{8}</td><td>[0-9a-f]{32}</td>"
    r"<td>([0-9a-f]{40})</td></tr>")


def _text(fragment: str) -> str:
    return " ".join(html.unescape(re.sub(r"<[^>]*>", " ", fragment)).split())


def read_disc_page(redump_id: int, text: str) -> Disc:
    info_start = text.find('<table class="gameinfo"')
    if info_start < 0:
        raise ValueError(f"disc {redump_id}: no gameinfo table")
    info_end = text.find("</table>", info_start)
    info = {k.strip(): v for k, v in _INFO_ROW.findall(text[info_start:info_end])}
    tracks = _TRACK_ROW.findall(text)
    return Disc(
        redump_id=redump_id,
        system=_text(info.get("System", "")),
        serial=_text(info.get("Serial", "")),
        region=", ".join(html.unescape(t) for t in _IMG_TITLE.findall(info.get("Region", ""))),
        edition=_text(info.get("Edition", "")),
        version=_text(info.get("Version", "")),
        sha1=bytes.fromhex(tracks[0][1]) if len(tracks) == 1 else b"",
    )


# ---- Wikidata: games, and the facts ADR 0014 takes ------------------------------------

# The English name of ?v: its "en" label, else its "mul" (default for all languages) label.
# Wikidata moves names that are the same in every language to "mul" and drops the "en"
# copy; Halo: Combat Evolved has only "mul" (2026-10-20), so "en" alone misses games.
# Labels are read directly, not through the label service, so a missing name is missing
# rather than replaced by the Q number.
_NAME = """
  OPTIONAL { ?v rdfs:label ?en. FILTER(LANG(?en) = "en") }
  OPTIONAL { ?v rdfs:label ?mul. FILTER(LANG(?mul) = "mul") }
  BIND(COALESCE(?en, ?mul) AS ?label)
  FILTER(BOUND(?label))"""

_XBOX_GAME = "?item wdt:P31 wd:Q7889; wdt:P400 wd:Q132020"  # video game (Q7889) for the Xbox

WIKIDATA_QUERIES = {
    # Xbox games and their names.
    "games": f"SELECT ?item ?label WHERE {{\n  {_XBOX_GAME}. BIND(?item AS ?v){_NAME}\n}}",
    # The names of their developers (P178), publishers (P123) and genres (P136).
    "developers": f"SELECT ?item ?label WHERE {{\n  {_XBOX_GAME}; wdt:P178 ?v.{_NAME}\n}}",
    "publishers": f"SELECT ?item ?label WHERE {{\n  {_XBOX_GAME}; wdt:P123 ?v.{_NAME}\n}}",
    "genres": f"SELECT ?item ?label WHERE {{\n  {_XBOX_GAME}; wdt:P136 ?v.{_NAME}\n}}",
    "dates": """
SELECT ?item ?label WHERE {
  ?item wdt:P31 wd:Q7889; wdt:P400 wd:Q132020; wdt:P577 ?label.
}""",
    "redump": """
SELECT ?item ?label WHERE {
  ?item wdt:P31 wd:Q7889; wdt:P400 wd:Q132020; wdt:P5290 ?label.
}""",
}

_ITEM = re.compile(r"http://www\.wikidata\.org/entity/Q([1-9]\d{0,9})")


def read_wikidata(name: str, text: str) -> list[tuple[int, str]]:
    """(Q number, value) pairs from one SPARQL JSON answer, sorted; bad rows skipped."""
    rows = json.loads(text)["results"]["bindings"]
    out = set()
    for row in rows:
        m = _ITEM.fullmatch(row.get("item", {}).get("value", ""))
        value = row.get("label", {}).get("value")
        if m and isinstance(value, str) and value.strip():
            q = int(m.group(1))
            if q < 2**32:
                out.add((q, value.strip()))
    return sorted(out)
