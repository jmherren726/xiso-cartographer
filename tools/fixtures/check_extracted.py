"""Check an extracted directory tree against a fixture manifest.

    python tools/fixtures/check_extracted.py <manifest.json> <extracted-dir>

Used to validate the generator against an independent reader (extract-xiso),
and later to check our own extractor's output. Exits non-zero on any mismatch:
missing, extra or wrongly sized files, wrong hashes, missing directories.
"""

from __future__ import annotations

import hashlib
import json
import os
import sys

CHUNK = 1 << 20


def sha256(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(CHUNK), b""):
            h.update(block)
    return h.hexdigest()


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    with open(argv[0], encoding="utf-8") as f:
        m = json.load(f)
    root = argv[1]
    problems = []

    expected_files = {e["path"]: e for e in m["files"]}
    expected_dirs = {e["path"] for e in m["dirs"]}
    for path, e in expected_files.items():
        p = os.path.join(root, *path.split("/"))
        if not os.path.isfile(p):
            problems.append(f"missing file: {path}")
            continue
        size = os.path.getsize(p)
        if size != e["size"]:
            problems.append(f"size mismatch: {path}: {size} != {e['size']}")
        elif sha256(p) != e["sha256"]:
            problems.append(f"content mismatch: {path}")
    for path in expected_dirs:
        if not os.path.isdir(os.path.join(root, *path.split("/"))):
            problems.append(f"missing directory: {path}")

    for dirpath, dirnames, filenames in os.walk(root):
        rel = os.path.relpath(dirpath, root).replace(os.sep, "/")
        prefix = "" if rel == "." else rel + "/"
        for d in dirnames:
            if prefix + d not in expected_dirs:
                problems.append(f"unexpected directory: {prefix + d}")
        for name in filenames:
            if prefix + name not in expected_files:
                problems.append(f"unexpected file: {prefix + name}")

    for p in problems:
        print(f"{m['name']}: {p}")
    status = "FAIL" if problems else "ok"
    print(f"{m['name']}: {status} ({len(expected_files)} files, {len(expected_dirs)} dirs)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
