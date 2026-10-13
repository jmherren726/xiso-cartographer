"""Extract a fixture with xcat-cli and check the result against its manifest.

    python tools/fixtures/extract_and_check.py --cli <xcat-cli> --fixtures <dir> --out <dir> NAME

The manifests were already checked against extract-xiso (verify.sh), so a pass
means our extractor's output matches extract-xiso's, file by file (size and
SHA-256), with the same directories and no extra files. Used by CTest
(extract_<name> tests).
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import check_extracted  # noqa: E402


def long_path(path: str) -> str:
    """On Windows, use the long-path prefix so 255-character names work."""
    if os.name == "nt":
        path = os.path.abspath(path)
        if not path.startswith("\\\\?\\"):
            path = "\\\\?\\" + path
    return path


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cli", required=True)
    ap.add_argument("--fixtures", required=True)
    ap.add_argument("--out", required=True, help="parent directory; NAME/ is replaced")
    ap.add_argument("name")
    args = ap.parse_args(argv)

    target = os.path.join(args.out, args.name)
    if os.path.exists(long_path(target)):
        shutil.rmtree(long_path(target))
    os.makedirs(args.out, exist_ok=True)
    image = os.path.join(args.fixtures, f"{args.name}.iso")
    result = subprocess.run([args.cli, "extract", image, target], check=False)
    if result.returncode != 0:
        print(f"{args.name}: xcat-cli extract failed with {result.returncode}")
        return 1
    manifest = os.path.join(args.fixtures, f"{args.name}.json")
    status = check_extracted.main([manifest, long_path(target)])
    shutil.rmtree(long_path(target))  # large fixtures extract to GBs
    return status


if __name__ == "__main__":
    sys.exit(main())
