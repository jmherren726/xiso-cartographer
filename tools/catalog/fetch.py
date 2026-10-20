"""Fetches the catalog's raw inputs into a cache outside the repo (ADR 0014 decisions 5, 7).

    python3 -I tools/catalog/fetch.py --cache <dir> [--refresh-discs]

A deliberate, occasional step, never part of a build or of CI. It asks Redump one
request at a time, at least 1 s apart, with a User-Agent that names the project, and
keeps every answer. Disc pages already in the cache are not fetched again unless
--refresh-discs is given, so a refresh costs the datfile, the list pages and the new
discs only. Then run build.py, which hashes what it reads into inputs.lock.

Cache layout:
    meta.json                    when each source was fetched
    redump/datfile.zip           http://redump.org/datfile/xbox/
    redump/list/<page>.html      http://redump.org/discs/system/xbox/?page=<page>
    redump/disc/<id>.html        http://redump.org/disc/<id>/
    wikidata/<query>.json        sources.WIKIDATA_QUERIES, from query.wikidata.org
"""
from __future__ import annotations

import argparse
import datetime
import json
import os
import sys
import time
import urllib.parse
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sources  # noqa: E402

USER_AGENT = ("xiso-cartographer-catalog/1 (+https://github.com/jmherren726/xiso-cartographer; "
              "occasional catalog refresh, one request at a time)")
REDUMP = "http://redump.org"  # redump.org serves no HTTPS
WIKIDATA = "https://query.wikidata.org/sparql"
MIN_INTERVAL = 1.0  # seconds between requests to one host (ADR 0014 decision 7)
MAX_BODY = 64 * 1024 * 1024
MAX_PAGES = 100


class Polite:
    """One request at a time per host, MIN_INTERVAL apart, with backoff on errors."""

    def __init__(self) -> None:
        self.last: dict[str, float] = {}

    def get(self, url: str, accept: str = "*/*") -> bytes:
        host = urllib.parse.urlsplit(url).hostname or ""
        for attempt in range(4):
            wait = self.last.get(host, 0.0) + MIN_INTERVAL * (4 ** attempt) - time.monotonic()
            if wait > 0:
                time.sleep(wait)
            req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT, "Accept": accept})
            try:
                with urllib.request.urlopen(req, timeout=60) as r:
                    body = r.read(MAX_BODY + 1)
                if len(body) > MAX_BODY:
                    raise ValueError(f"{url}: answer larger than {MAX_BODY} bytes")
                return body
            except (OSError, urllib.error.URLError) as e:
                print(f"  {url}: {e} (attempt {attempt + 1})", file=sys.stderr)
            finally:
                self.last[host] = time.monotonic()
        raise SystemExit(f"giving up on {url}")


def write(path: str, data: bytes) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".part"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cache", required=True, help="cache directory, outside the repo")
    ap.add_argument("--refresh-discs", action="store_true", help="fetch cached disc pages again")
    args = ap.parse_args()
    cache = os.path.abspath(args.cache)
    http = Polite()
    today = datetime.date.today().isoformat()

    print("Wikidata")
    for name, query in sorted(sources.WIKIDATA_QUERIES.items()):
        url = WIKIDATA + "?" + urllib.parse.urlencode({"query": query, "format": "json"})
        body = http.get(url, "application/sparql-results+json")
        print(f"  {name}: {len(sources.read_wikidata(name, body.decode('utf-8')))} rows")
        write(os.path.join(cache, "wikidata", name + ".json"), body)

    print("Redump datfile")
    dat = http.get(REDUMP + "/datfile/xbox/")
    version, entries = sources.read_datfile(dat)
    print(f"  version {version}, {len(entries)} discs")
    write(os.path.join(cache, "redump", "datfile.zip"), dat)

    print("Redump list pages")
    ids: set[int] = set()
    page, last = 1, 1
    while page <= last:
        body = http.get(f"{REDUMP}/discs/system/xbox/?page={page}")
        page_ids, linked = sources.read_list_page(body.decode("utf-8"))
        last = min(max(last, linked), MAX_PAGES)
        write(os.path.join(cache, "redump", "list", f"{page}.html"), body)
        ids.update(page_ids)
        page += 1
    print(f"  {last} pages, {len(ids)} discs")

    todo = [i for i in sorted(ids)
            if args.refresh_discs or not os.path.exists(os.path.join(cache, "redump", "disc", f"{i}.html"))]
    print(f"Redump disc pages: {len(todo)} to fetch, about {len(todo) * MIN_INTERVAL / 60:.0f} min")
    for n, i in enumerate(todo, 1):
        write(os.path.join(cache, "redump", "disc", f"{i}.html"), http.get(f"{REDUMP}/disc/{i}/"))
        if n % 100 == 0:
            print(f"  {n}/{len(todo)}")

    meta = {"wikidata": today, "redump": today, "redump_datfile": version}
    write(os.path.join(cache, "meta.json"), (json.dumps(meta, indent=1, sort_keys=True) + "\n").encode())
    print("Done. Next: build.py --cache", cache, "--update-lock")


if __name__ == "__main__":
    main()
