"""HTTP/1.1 test server with Range support and scripted faults.

    python tools/testserver/rangeserver.py --root DIR [--bind 127.0.0.1] [--port 8080]

Serves the files in DIR (read-only, no listings) the way docs/formats/http.md
expects a good server to: single byte ranges with 206 and Content-Range,
keep-alive, ETag and Last-Modified. Query parameters on any URL make it
misbehave in controlled ways, so tests can check that the client survives (or
correctly refuses) each case:

    redirect=N      answer 302 N times in a row before serving (Location is relative)
    auth=VALUE      answer 401 unless the request has "Authorization: VALUE"
    status=CODE     answer CODE (e.g. 503, 403) instead of the file ...
    retry_after=S   ... with Retry-After: S
    drop=N          close the connection after N body bytes ...
    stall=MS        ... or, instead, stop sending for MS ms after `at` body bytes, then close
    at=N            where a stall starts (default 0)
    times=K         how many responses status/drop/stall affect (default 1; 0 = all)
    norange=1       ignore Range and answer 200 with the whole file
    chunked=1       send 206 bodies with Transfer-Encoding: chunked
    change=1        the file "changes" (new ETag) after the first response
    close=1         send Connection: close on every response
    rate=BPS        limit the body to BPS bytes per second

A fault counts down only on responses it actually affects: drop=1000 skips the
1-byte probe (`Range: bytes=0-0`) and hits the first body longer than 1000 bytes.
Counters are per URL (ignoring `redirect`), for the life of the server.

The tests (test_get.py) run it in-process; for the console's network test, run
it from the command line with --bind 0.0.0.0 so the Xbox can reach it.
"""

from __future__ import annotations

import argparse
import email.utils
import os
import re
import socket
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

CHUNK = 64 * 1024
RANGE_RE = re.compile(r"bytes=(\d+)-(\d*)\Z")


class RangeServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, root: str, bind: str = "127.0.0.1", port: int = 0, verbose: bool = False):
        self.root = os.path.abspath(root)
        self.verbose = verbose
        self.lock = threading.Lock()
        self.counters: dict[tuple[str, str], int] = {}
        self.requests: list[dict[str, str]] = []  # every request head, for tests to inspect
        super().__init__((bind, port), Handler)

    @property
    def port(self) -> int:
        return self.server_address[1]

    def take(self, key: str, fault: str, times: int) -> bool:
        """True if fault `fault` should hit this response (and counts it)."""
        with self.lock:
            used = self.counters.get((key, fault), 0)
            if times != 0 and used >= times:
                return False
            self.counters[(key, fault)] = used + 1
            return True

    def bump(self, key: str, what: str) -> int:
        """Increments a plain counter and returns its previous value."""
        with self.lock:
            n = self.counters.get((key, what), 0)
            self.counters[(key, what)] = n + 1
            return n

    def start(self) -> threading.Thread:
        t = threading.Thread(target=self.serve_forever, daemon=True)
        t.start()
        return t


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server: RangeServer

    def log_message(self, format: str, *args: object) -> None:  # noqa: A002 (stdlib name)
        if self.server.verbose:
            super().log_message(format, *args)

    # --- helpers ---------------------------------------------------------------------------

    def head(self, status: int, headers: list[tuple[str, str]], close: bool) -> None:
        self.send_response(status)
        for name, value in headers:
            self.send_header(name, value)
        if close:
            self.send_header("Connection", "close")
            self.close_connection = True
        self.end_headers()

    def empty(self, status: int, headers: list[tuple[str, str]], close: bool) -> None:
        self.head(status, headers + [("Content-Length", "0")], close)

    # --- GET -------------------------------------------------------------------------------

    def do_GET(self) -> None:  # noqa: N802 (stdlib name)
        srv = self.server
        srv.requests.append({k.lower(): v for k, v in self.headers.items()} | {":target": self.path})
        url = urllib.parse.urlsplit(self.path)
        q = {k: v[-1] for k, v in urllib.parse.parse_qs(url.query).items()}
        key_q = urllib.parse.urlencode(sorted((k, v) for k, v in q.items() if k != "redirect"))
        key = url.path + "?" + key_q
        close = q.get("close") == "1"
        times = int(q.get("times", "1"))

        redirect = int(q.get("redirect", "0"))
        if redirect > 0:
            nq = dict(q, redirect=str(redirect - 1))
            name = url.path.rsplit("/", 1)[-1]
            self.empty(302, [("Location", urllib.parse.quote(name) + "?" + urllib.parse.urlencode(nq))], close)
            return

        if "auth" in q and self.headers.get("Authorization") != q["auth"]:
            self.empty(401, [], close)
            return

        if "status" in q and srv.take(key, "status", times):
            headers = []
            if "retry_after" in q:
                headers.append(("Retry-After", q["retry_after"]))
            self.empty(int(q["status"]), headers, close)
            return

        rel = urllib.parse.unquote(url.path).lstrip("/")
        path = os.path.abspath(os.path.join(srv.root, rel))
        if not path.startswith(srv.root + os.sep) or not os.path.isfile(path):
            self.empty(404, [], close)
            return
        st = os.stat(path)
        size = st.st_size
        etag = f'"{size:x}-{int(st.st_mtime):x}'
        if q.get("change") == "1" and srv.bump(key, "change") > 0:
            etag += "-new"
        etag += '"'
        validators = [("ETag", etag), ("Last-Modified", email.utils.formatdate(st.st_mtime, usegmt=True))]

        rng = self.headers.get("Range")
        if rng is None or q.get("norange") == "1":
            first, last, status = 0, size - 1, 200
        else:
            m = RANGE_RE.match(rng.strip())
            if m is None:
                self.empty(400, [], close)
                return
            first = int(m.group(1))
            last = int(m.group(2)) if m.group(2) else size - 1
            if first >= size or last < first:
                self.empty(416, [("Content-Range", f"bytes */{size}")] + validators, close)
                return
            last = min(last, size - 1)
            status = 206
        length = last - first + 1 if size > 0 else 0

        drop = stall = None
        if "drop" in q and length > int(q["drop"]) and srv.take(key, "drop", times):
            drop = int(q["drop"])
        elif "stall" in q and length > int(q.get("at", "0")) and srv.take(key, "stall", times):
            stall = int(q.get("at", "0"))
        chunked = status == 206 and q.get("chunked") == "1"

        headers = list(validators) + [("Accept-Ranges", "bytes")]
        if status == 206:
            headers.append(("Content-Range", f"bytes {first}-{last}/{size}"))
        headers.append(("Transfer-Encoding", "chunked") if chunked else ("Content-Length", str(length)))
        self.head(status, headers, close)  # a drop or stall comes without warning

        cut = drop if drop is not None else stall
        rate = int(q.get("rate", "0"))
        sent = 0
        start = time.monotonic()
        try:
            with open(path, "rb") as f:
                f.seek(first)
                while sent < length:
                    n = min(CHUNK, length - sent)
                    if cut is not None:
                        n = min(n, cut - sent)
                        if n <= 0:
                            break
                    data = f.read(n)
                    if chunked:
                        self.wfile.write(f"{len(data):x}\r\n".encode() + data + b"\r\n")
                    else:
                        self.wfile.write(data)
                    sent += len(data)
                    if rate > 0:
                        ahead = sent / rate - (time.monotonic() - start)
                        if ahead > 0:
                            time.sleep(ahead)
                if cut is None and chunked:
                    self.wfile.write(b"0\r\n\r\n")
                self.wfile.flush()
                if stall is not None:
                    time.sleep(int(q["stall"]) / 1000.0)
        except (ConnectionError, socket.timeout):
            self.close_connection = True
            return
        if cut is not None:
            self.close_connection = True
            try:
                self.connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", required=True, help="directory to serve")
    ap.add_argument("--bind", default="127.0.0.1", help="address to listen on (0.0.0.0: all)")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("-q", "--quiet", action="store_true", help="don't log requests")
    args = ap.parse_args(argv)
    srv = RangeServer(args.root, args.bind, args.port, verbose=not args.quiet)
    print(f"serving {srv.root} on http://{args.bind}:{srv.port}/ (Ctrl+C to stop)", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    srv.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
