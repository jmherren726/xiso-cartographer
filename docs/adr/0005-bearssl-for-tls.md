# 0005. Use BearSSL for on-box TLS

- **Status:** Accepted; amended by [ADR 0012](0012-tls-trust-time-and-entropy.md) (CA bundle source and override, randomness on the Xbox, and the console's clock as the fallback when NTP is unreachable)
- **Date:** 2026-10-13

## Context

archive.org redirects every plain-HTTP request to HTTPS, including requests to its storage servers (HANDOFF §2). So installing from archive.org without a PC needs a TLS client running on the Xbox: a ~733 MHz Pentium III-class CPU with no AES instructions, 64 MB RAM shared with everything else, and nxdk's C runtime (PDCLib) plus lwIP for sockets.

Tests from a PC showed archive.org accepts TLS 1.2 (`curl --tls-max 1.2` → `206`), so TLS 1.3 isn't required today.

Candidates:

| | BearSSL | Mbed TLS | wolfSSL |
|---|---|---|---|
| Licence | MIT | Apache-2.0 | GPL-2.0 or commercial |
| TLS versions | 1.0–1.2 | 1.2, 1.3 | 1.2, 1.3 |
| Memory model | No dynamic allocation; the caller provides every buffer | Uses heap allocation (hooks available) | Uses heap allocation (static option) |
| Code size | Small | Larger | Medium |
| Needs from the C library | Very little (`memcpy`/`memset`-level) | More (config needed for platform support) | More |
| Development activity | Slow; last tagged release is old | Active | Active |

## Decision

Use **BearSSL**, pinned as a submodule when M5 starts, behind a thin `tls` module in `core/` (`xc_tls_*`). Nothing outside that module calls BearSSL directly, except the hash functions used for verification ([ADR 0004](0004-streaming-install.md)).

Configuration rules:

- **TLS 1.2 only**, with SNI, and certificate validation always on (HANDOFF §7.3). There's no option to disable validation, not even in debug builds.
- **Prefer ChaCha20-Poly1305** cipher suites, then AES-GCM. Without AES instructions, ChaCha20 is usually the faster choice in software. Confirm with the M5 benchmark.
- **Trust anchors:** a CA bundle built from the Mozilla root store at build time and shipped in the release. It can be replaced by a file on the HDD, so roots can be updated without a new XBE.
- **Time:** certificate validity needs a correct clock, which a console's clock often isn't. The `ntp` module sets the time over SNTP before the first TLS connection. If no trusted time is available, HTTPS fails with a clear message, rather than skipping the date check.
- **Buffers:** one I/O buffer per connection, allocated up front at job start (the full-size TLS record buffer is about 33 KB).

## Consequences

- Small, allocation-free TLS that fits the "fixed buffers, allocate up front" rule (HANDOFF §7.2) and the MIT licence ([ADR 0002](0002-license.md)).
- BearSSL also provides MD5 and SHA-1 for verification, so no separate hash library is needed.
- **Risk: no TLS 1.3.** If archive.org (or a user's server) ever requires TLS 1.3, BearSSL can't connect. Because callers only use `xc_tls_*`, Mbed TLS can replace it in one module. We would then revisit memory use. M5 records which TLS version and cipher suite archive.org negotiates, so we notice a change early.
- **Risk: slow upstream.** Security fixes may be slow to arrive. We track upstream and the CVE feeds at each release. The submodule pin makes every upgrade explicit.
- **Performance is the main unknown.** M5's go/no-go measurement (target ≥ 1 MB/s on hardware) decides whether the Xbox-only design holds (HANDOFF §4.1).
