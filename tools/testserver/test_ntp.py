"""End-to-end tests for `xcat-cli ntp` against the fake SNTP server (sntpserver.py).

    python tools/testserver/test_ntp.py --list
    python tools/testserver/test_ntp.py --cli <xcat-cli> <scenario>

Each scenario starts the server in one mode, runs `xcat-cli ntp`, and checks the exit status,
the output and how many requests the server saw (docs/formats/sntp.md sections 4 and 6). Used by
CTest (ntp_<scenario> tests).
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import sntpserver  # noqa: E402

FLOOR_2026 = "1790000000"  # 2026-09-21: below the server's time, above the "early" one

# name: (server mode, extra args, exit status, output pattern, requests the server saw)
SCENARIOS = {
    "good": ("good", [], 0, r"2026-10-17 00:00:0\d UTC, stratum 2, .*, 1 request\n", 1),
    "lost_first": ("lost-first", [], 0, r"2026-10-17 .*, 2 requests\n", 2),
    "stray": ("stray", ["--floor", FLOOR_2026], 0, r"2026-10-17 .*, 1 request\n", 1),
    "kiss": ("kiss", [], 1, r"refused.*: kiss-o'-death RATE \(1 request\)", 1),
    "unsynced": ("unsynced", [], 1, r"server not synchronized \(3 requests\)", 3),
    "wrong_nonce": ("wrong-nonce", [], 1, r"answer to a different request \(3 requests\)", 3),
    "early": ("early", ["--floor", FLOOR_2026], 1, r"earlier than this program's build \(1 request\)", 1),
    "silent": ("silent", [], 1, r"\(3 requests\)", 3),
}


def run(cli: str, name: str) -> int:
    mode, extra, want_rc, pattern, want_requests = SCENARIOS[name]
    srv = sntpserver.SntpServer(mode)
    srv.start()
    try:
        cmd = [cli, "ntp", "--port", str(srv.port), "--timeout", "400", *extra, "127.0.0.1"]
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=60, check=False)
    finally:
        srv.stop()
    out = result.stdout + result.stderr
    print(out, end="")
    ok = True
    if result.returncode != want_rc:
        print(f"FAIL: exit status {result.returncode}, expected {want_rc}")
        ok = False
    if not re.search(pattern, out):
        print(f"FAIL: output doesn't match {pattern!r}")
        ok = False
    if srv.requests != want_requests:
        print(f"FAIL: the server saw {srv.requests} requests, expected {want_requests}")
        ok = False
    return 0 if ok else 1


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--list", action="store_true", help="print the scenario names")
    ap.add_argument("--cli")
    ap.add_argument("scenario", nargs="?", choices=sorted(SCENARIOS))
    args = ap.parse_args(argv)
    if args.list:
        print(";".join(SCENARIOS), end="")
        return 0
    if args.cli is None or args.scenario is None:
        ap.error("--cli and a scenario are required")
    return run(args.cli, args.scenario)


if __name__ == "__main__":
    raise SystemExit(main())
