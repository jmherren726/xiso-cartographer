"""Fake SNTP server for tests (docs/formats/sntp.md): answers on 127.0.0.1 with a fixed time,
or misbehaves on purpose.

    python tools/testserver/sntpserver.py [--port N] [--mode good]

Modes (one per check of sntp.md section 4, plus lost and stray datagrams):

    good         a valid answer: 2026-10-17 00:00:00.5 UTC, stratum 2
    lost-first   ignores the first request, answers the next
    stray        first sends a valid-looking answer from another port, then the real one
    kiss         kiss-o'-death RATE (stratum 0)
    unsynced     leap indicator 3 (not synchronized)
    wrong-nonce  echoes a different originate timestamp
    early        a valid answer with a 2001 time (below any build-time floor)
    silent       never answers

Used by test_ntp.py; can also be run by hand against `xcat-cli ntp --port N 127.0.0.1`.
"""

from __future__ import annotations

import argparse
import socket
import struct
import threading

NTP_UNIX = 2208988800
T_UNIX = 1792195200  # 2026-10-17 00:00:00 UTC
EARLY_UNIX = 1000000000  # 2001-09-09
MODES = ("good", "lost-first", "stray", "kiss", "unsynced", "wrong-nonce", "early", "silent")


def answer(req: bytes, mode: str) -> bytes:
    nonce = req[40:48]
    t = (EARLY_UNIX if mode == "early" else T_UNIX) + NTP_UNIX
    b0, stratum, refid = 0x24, 2, b"GPS\0"  # LI 0, VN 4, Mode 4
    if mode == "kiss":
        stratum, refid = 0, b"RATE"
    if mode == "unsynced":
        b0 = 0xE4
    if mode == "wrong-nonce":
        nonce = bytes(x ^ 0xFF for x in nonce)
    return (
        bytes([b0, stratum, 6, 0xEC])
        + struct.pack(">II", 0, 0)
        + refid
        + struct.pack(">II", t - 30, 0)
        + nonce
        + struct.pack(">II", t, 0)
        + struct.pack(">II", t, 0x80000000)
    )


class SntpServer:
    def __init__(self, mode: str, port: int = 0) -> None:
        if mode not in MODES:
            raise ValueError(mode)
        self.mode = mode
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", port))
        self.sock.settimeout(0.2)
        self.port = self.sock.getsockname()[1]
        self.requests = 0
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)

    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        self._thread.join(timeout=5)
        self.sock.close()

    def _run(self) -> None:
        while not self._stop.is_set():
            try:
                req, peer = self.sock.recvfrom(512)
            except (TimeoutError, socket.timeout):
                continue
            except OSError:
                continue  # Windows reports an earlier ICMP error here; keep serving
            self.requests += 1
            if len(req) < 48 or self.mode == "silent":
                continue
            if self.mode == "lost-first" and self.requests == 1:
                continue
            if self.mode == "stray":
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as other:
                    other.sendto(answer(req, "early"), peer)
            self.sock.sendto(answer(req, self.mode), peer)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=12300)
    ap.add_argument("--mode", choices=MODES, default="good")
    args = ap.parse_args()
    srv = SntpServer(args.mode, args.port)
    print(f"SNTP ({args.mode}) on 127.0.0.1:{srv.port}; Ctrl+C stops", flush=True)
    srv.start()
    try:
        threading.Event().wait()
    except KeyboardInterrupt:
        pass
    srv.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
