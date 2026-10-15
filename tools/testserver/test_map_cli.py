"""End-to-end test for `xcat-cli map`: local and HTTP includes, problems, exit status.

    python tools/testserver/test_map_cli.py --cli <xcat-cli> --work <dir>

Writes a top-level map that includes a local file and a map served over HTTP by
rangeserver.py (which in turn includes a relative URL), plus one bad line, and
checks the listed entries, the reported problem and the exit status. Used by
CTest (map_cli test).
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import rangeserver  # noqa: E402

HDR = "# xcat-map v1\n"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cli", required=True)
    ap.add_argument("--work", required=True)
    args = ap.parse_args(argv)

    work = os.path.join(args.work, "map-cli")
    shutil.rmtree(work, ignore_errors=True)
    served = os.path.join(work, "served", "maps")
    os.makedirs(served)
    with open(os.path.join(served, "remote.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write(HDR + "00000003 = http://192.168.1.20:8080/three.iso\n@include more/nested.txt\n")
    os.makedirs(os.path.join(served, "more"))
    with open(os.path.join(served, "more", "nested.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write(HDR + "00000004@PAL = http://192.168.1.20:8080/four (Europe).iso\n")

    srv = rangeserver.RangeServer(os.path.join(work, "served"))
    srv.start()
    try:
        local = os.path.join(work, "local")
        os.makedirs(local)
        with open(os.path.join(local, "sub.txt"), "w", encoding="utf-8", newline="\n") as f:
            f.write(HDR + "00000002 = http://192.168.1.20:8080/two.iso\n")
        top = os.path.join(local, "map.txt")
        with open(top, "w", encoding="utf-8", newline="\n") as f:
            f.write(
                HDR
                + "00000001 = http://192.168.1.20:8080/one.iso\n"
                + "@include sub.txt\n"
                + f"@include http://127.0.0.1:{srv.port}/maps/remote.txt\n"
                + "not a valid line\n"
                + "00000001 = http://192.168.1.20:8080/one-again.iso\n"
            )
        result = subprocess.run([args.cli, "map", top], capture_output=True, text=True, timeout=60,
                                check=False)
    finally:
        srv.shutdown()
        srv.server_close()

    print(result.stdout + result.stderr, end="")
    problems = []
    want = [
        r"^00000001\s+http://192\.168\.1\.20:8080/one-again\.iso  \(.*map\.txt:6\)$",
        r"^00000002\s+http://192\.168\.1\.20:8080/two\.iso  \(.*sub\.txt:2\)$",
        rf"^00000003\s+http://192\.168\.1\.20:8080/three\.iso  \(http://127\.0\.0\.1:{srv.port}/maps/remote\.txt:2\)$",
        rf"^00000004@PAL\s+http://192\.168\.1\.20:8080/four \(Europe\)\.iso  \(http://127\.0\.0\.1:{srv.port}/maps/more/nested\.txt:2\)$",
        r"^4 entries from 4 files, 1 error$",
    ]
    for pattern in want:
        if re.search(pattern, result.stdout, re.MULTILINE) is None:
            problems.append(f"missing output line matching {pattern!r}")
    if re.search(r"map\.txt:5: error: expected", result.stderr) is None:
        problems.append("the bad line 5 isn't reported")
    if re.search(r"map\.txt:6: warning: 00000001 also defined at .*map\.txt:2", result.stderr) is None:
        problems.append("the duplicate on line 6 isn't reported")
    if result.returncode != 1:
        problems.append(f"exit status {result.returncode}, expected 1 (an error line)")
    shutil.rmtree(work, ignore_errors=True)
    for p in problems:
        print(f"FAIL map_cli: {p}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
