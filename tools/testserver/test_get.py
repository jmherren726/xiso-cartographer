"""End-to-end tests for `xcat-cli get` against rangeserver.py.

    python tools/testserver/test_get.py --cli <xcat-cli> --work <dir> SCENARIO
    python tools/testserver/test_get.py --list

Each scenario starts the server in-process on a free port, serving generated
test files, runs `xcat-cli get` against one URL (usually with faults, see
rangeserver.py) and checks the exit status, the messages, and that the
download is byte-identical to the served file (or, on failure, that no file
was left behind). Used by CTest (get_<scenario> tests).
"""

from __future__ import annotations

import argparse
import hashlib
import os
import random
import re
import shutil
import socket
import subprocess
import sys
import zlib
from dataclasses import dataclass, field

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import rangeserver  # noqa: E402

TLS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "fixtures", "tls")
BIG = 3 * 1024 * 1024 + 12345  # several receive buffers, not a round number


@dataclass
class Scenario:
    url: str  # path and query on the test server, or a full URL
    ok: bool = True
    stderr: str | None = None  # regex the messages must match
    args: list[str] = field(default_factory=list)
    file: str = "big.bin"  # which served file a success must match
    check: str | None = None  # name of an extra check on the server's request log
    creds: str | None = None  # credentials file to pass with --credentials ("-": a missing file)
    tls: bool = False  # serve HTTPS with the test PKI; a path URL becomes https://localhost:<port>
    ca: str | None = "ca.pem"  # with tls: the --ca-file from tools/fixtures/tls/ (None: built-in)


# A LAN-style section for the test server. 127.0.0.1 isn't a private range (credentials.md §6),
# so over plain http the token must never be sent, even with allow_http.
LOOPBACK_CREDS = (
    "# xcat-credentials v1\n[lan]\nhosts = 127.0.0.1\nallow_http = yes\n"
    "header = Authorization: Bearer test-token\n"
)
BAD_CREDS = "# xcat-credentials v1\n[lan]\nhosts = 127.0.0.1:*\nheader = Authorization: Bearer x\n"

# --timeout 1000 keeps stalls short; the backoff (0.5-1 s, then 1-2 s) is the client's own.
FAST = ["--timeout", "1000"]

SCENARIOS: dict[str, Scenario] = {
    "plain": Scenario("/big.bin", check="request_format"),
    "small": Scenario("/small.bin", file="small.bin"),
    "empty": Scenario("/empty.bin", file="empty.bin"),
    "spaces": Scenario("/with%20space.bin", file="with space.bin"),
    "keepalive": Scenario("/big.bin", check="one_connection"),
    "no_keepalive": Scenario("/big.bin?close=1"),
    "chunked": Scenario("/big.bin?chunked=1"),
    "redirects": Scenario("/big.bin?redirect=5"),
    "too_many_redirects": Scenario("/big.bin?redirect=6", ok=False, stderr="Too many redirects"),
    "drop": Scenario("/big.bin?drop=100000&times=2", stderr="network connection failed, retrying"),
    "drop_chunked": Scenario("/big.bin?drop=100000&chunked=1", stderr="retrying"),
    "drop_resumes": Scenario("/big.bin?drop=1500000", check="resumed_range"),
    "stall": Scenario("/big.bin?stall=3000&at=200000", args=FAST, stderr="stopped responding, retrying"),
    "status_503": Scenario("/big.bin?status=503", stderr="HTTP 503, retrying"),
    "retry_after": Scenario("/big.bin?status=429&retry_after=1", stderr=r"HTTP 429, retrying in 1\.0 s"),
    "gives_up": Scenario("/big.bin?status=503&times=0", ok=False, args=["--attempts", "2"], stderr=r"HTTP 503\)"),
    "no_range": Scenario("/big.bin?norange=1", ok=False, stderr="support resumable downloads"),
    "not_found": Scenario("/missing.bin", ok=False, stderr=r"wasn't found.*HTTP 404"),
    "denied": Scenario("/big.bin?status=403&times=0", ok=False, stderr=r"Access denied.*HTTP 403"),
    "changed": Scenario("/big.bin?change=1", ok=False, stderr="changed during the download"),
    "refused": Scenario("http://127.0.0.1:{closed_port}/big.bin", ok=False,
                        args=["--attempts", "2"], stderr="Couldn't connect"),
    "dns": Scenario("http://xcat-test.invalid/big.bin", ok=False, args=["--attempts", "1"],
                    stderr="name lookup failed"),
    # HTTPS (xcat/tls.h, ADR 0012) against the test PKI: certificate for test.example and localhost.
    "https": Scenario("/big.bin", tls=True, check="one_connection"),
    "https_redirects": Scenario("/big.bin?redirect=3", tls=True),
    "https_drop_resumes": Scenario("/big.bin?drop=1500000", tls=True, check="resumed_range"),
    "https_chunked": Scenario("/big.bin?chunked=1", tls=True),
    "https_unknown_ca": Scenario("/big.bin", tls=True, ca="other-ca.pem", ok=False,
                                 stderr=r"certificate isn't trusted .*TLS error \d+", check="not_retried"),
    "https_builtin_ca": Scenario("/big.bin", tls=True, ca=None, ok=False, stderr="certificate isn't trusted",
                                 check="not_retried"),
    "https_wrong_name": Scenario("https://127.0.0.1:{port}/big.bin", tls=True, ok=False,
                                 stderr="certificate isn't trusted", check="not_retried"),
    "https_clock_reset": Scenario("/big.bin", tls=True, args=["--now", "978307200"], ok=False,
                                  stderr=r"is the clock right\?", check="not_retried"),
    "https_expired": Scenario("/big.bin", tls=True, args=["--now", "2082758401"], ok=False,
                              stderr=r"is the clock right\?"),
    "https_bad_ca_file": Scenario("/big.bin", tls=True, ca="make_certs.sh", ok=False,
                                  stderr="make_certs.sh: not a valid PEM certificate bundle"),
    "http_to_tls_port": Scenario("http://localhost:{port}/big.bin", tls=True, ok=False, args=["--attempts", "1"]),
    "bad_url": Scenario("ftp://127.0.0.1/big.bin", ok=False, stderr="Invalid or unsupported URL"),
    "creds_not_over_http": Scenario("/big.bin?auth=Bearer%20test-token", ok=False, creds=LOOPBACK_CREDS,
                                    stderr=r"Access denied.*HTTP 401", check="no_authorization"),
    "creds_unused_host": Scenario("/big.bin", creds=LOOPBACK_CREDS.replace("127.0.0.1", "192.168.77.7"),
                                  check="no_authorization"),
    "creds_bad_file": Scenario("/big.bin", creds=BAD_CREDS, check="no_authorization",
                               stderr=r"line 3: \[lan\]: invalid host pattern; continuing without credentials"),
    "creds_missing_file": Scenario("/big.bin", creds="-", stderr="can't read it; continuing without credentials"),
}


def make_files(root: str) -> None:
    os.makedirs(root, exist_ok=True)
    rnd = random.Random(0x5EED)
    files = {"big.bin": BIG, "small.bin": 1000, "empty.bin": 0, "with space.bin": 4096}
    for name, size in files.items():
        with open(os.path.join(root, name), "wb") as f:
            f.write(rnd.randbytes(size))


def sha256(path: str) -> str:
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def closed_port() -> int:
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def check_requests(name: str, srv: rangeserver.RangeServer) -> str | None:
    """Extra checks on what the client sent; returns a problem or None."""
    reqs = srv.requests
    if name == "request_format":
        r = reqs[0]
        if not r.get("user-agent", "").startswith("xiso-cartographer/"):
            return f"bad User-Agent: {r.get('user-agent')!r}"
        if r.get("accept-encoding") != "identity":
            return "no Accept-Encoding: identity"
        if r.get("range") != "bytes=0-0":
            return f"first request should probe with bytes=0-0, got {r.get('range')!r}"
        if not re.fullmatch(r"127\.0\.0\.1:\d+", r.get("host", "")):
            return f"bad Host: {r.get('host')!r}"
    if name == "one_connection" and srv.connections != 1:
        return f"expected one kept-alive connection, saw {srv.connections}"
    if name == "resumed_range":
        ranges = [r.get("range") for r in reqs]
        if ranges != ["bytes=0-0", f"bytes=0-{BIG - 1}", f"bytes=1500000-{BIG - 1}"]:
            return f"unexpected ranges: {ranges}"
    if name == "no_authorization" and any("authorization" in r for r in reqs):
        return "a credential was sent where credentials.md §6 forbids it"
    if name == "not_retried":
        if srv.connections != 1 or reqs:
            return f"a refused certificate must end it: {srv.connections} connections, {len(reqs)} requests"
    return None


class CountingServer(rangeserver.RangeServer):
    connections = 0

    def process_request(self, request, client_address):  # type: ignore[no-untyped-def]
        self.connections += 1
        super().process_request(request, client_address)


def run(cli: str, work: str, name: str) -> int:
    sc = SCENARIOS[name]
    # Everything per scenario, so CTest can run them in parallel.
    scratch = os.path.join(work, name)
    shutil.rmtree(scratch, ignore_errors=True)
    root = os.path.join(scratch, "served")
    make_files(root)
    out = os.path.join(scratch, "download.bin")
    args = list(sc.args)
    if sc.creds is not None:
        cred_file = os.path.join(scratch, "credentials")
        if sc.creds != "-":
            with open(cred_file, "w", encoding="utf-8", newline="\n") as f:
                f.write(sc.creds)
        args += ["--credentials", cred_file]

    tls = None
    if sc.tls:
        tls = rangeserver.tls_context(os.path.join(TLS_DIR, "server-chain.pem"),
                                      os.path.join(TLS_DIR, "server.key.der"))
        if sc.ca is not None:
            args += ["--ca-file", os.path.join(TLS_DIR, sc.ca)]
    srv = CountingServer(root, tls=tls)
    srv.start()
    try:
        url = sc.url.format(port=srv.port, closed_port=closed_port())
        if url.startswith("/"):
            url = f"https://localhost:{srv.port}{url}" if sc.tls else f"http://127.0.0.1:{srv.port}{url}"
        result = subprocess.run([cli, "get", *args, url, out], capture_output=True, text=True,
                                timeout=120, check=False)
    finally:
        srv.shutdown()
        srv.server_close()

    print(f"$ xcat-cli get {' '.join(args)} {url}")
    print(result.stdout + result.stderr, end="")
    problems = []
    if (result.returncode == 0) != sc.ok:
        problems.append(f"exit status {result.returncode}, expected {'0' if sc.ok else 'non-zero'}")
    if sc.stderr is not None and re.search(sc.stderr, result.stderr) is None:
        problems.append(f"messages don't match {sc.stderr!r}")
    if sc.ok and result.returncode == 0:
        if not os.path.isfile(out) or sha256(out) != sha256(os.path.join(root, sc.file)):
            problems.append("download differs from the served file")
        with open(os.path.join(root, sc.file), "rb") as f:
            want = f"crc32 {zlib.crc32(f.read()):08x}"
        if want not in result.stdout:
            problems.append(f"expected {want!r} in the summary")
        if sc.tls and "tls: TLS 1.2, ECDHE-ECDSA-" not in result.stdout:
            problems.append("expected the negotiated TLS 1.2 ECDHE-ECDSA suite in the summary")
    if not sc.ok and os.path.exists(out):
        problems.append("a failed download left a file behind")
    if sc.check is not None:
        p = check_requests(sc.check, srv)
        if p is not None:
            problems.append(p)
    shutil.rmtree(scratch, ignore_errors=True)
    for p in problems:
        print(f"FAIL {name}: {p}")
    return 1 if problems else 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cli")
    ap.add_argument("--work", help="scratch directory")
    ap.add_argument("--list", action="store_true", help="print the scenario names (for CMake)")
    ap.add_argument("scenario", nargs="?", choices=sorted(SCENARIOS))
    args = ap.parse_args(argv)
    if args.list:
        print(";".join(SCENARIOS))
        return 0
    if args.cli is None or args.work is None or args.scenario is None:
        ap.error("--cli, --work and a scenario are required")
    return run(args.cli, args.work, args.scenario)


if __name__ == "__main__":
    sys.exit(main())
