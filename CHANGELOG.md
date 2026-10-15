# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Repository scaffold: README, contributing guide, agent guide, handoff and plan documents, ADR process, MR template, code owners and Dependabot config.
- MIT licence (ADR 0002).
- Host build: CMake presets (`host-debug` with ASan/UBSan, `host-release`), warnings as errors, clang-format and clang-tidy configs.
- Unity unit test framework (ADR 0003) and a first core test.
- GitHub Actions CI: format check, clang-tidy, and build + tests on Linux (gcc, clang) and Windows (cl, clang-cl).
- nxdk toolchain (pinned submodule) in a Docker build image, and a hello-world XBE that runs libxcat on the Xbox. CI builds it and uploads `default.xbe` and a bootable ISO.
- M0 complete: the hello XBE boots in xemu and on real hardware (docs/testing.md).
- Map file format v1 specification (docs/formats/map.md) and architecture document (docs/ARCHITECTURE.md).
- ADRs 0004 (streaming install over HTTP Range), 0005 (BearSSL for TLS) and 0006 (no links, no bypasses, good-guest networking).
- Synthetic xiso fixture generator (tools/fixtures/): small, deep, XGD1/XGD2/XGD3 full-disc and >4 GiB images from dummy data, with manifests; deterministic, sparse, and cross-checked against extract-xiso in CI.
- XDVDFS reader (`xc_xdvdfs_open`, `xc_xdvdfs_list`): detects trimmed xiso and full-disc XGD1/2/3 images, walks directory trees without recursion, rejects corrupt or hostile images (docs/formats/xdvdfs.md), and lists files in offset order. Plus `xc_err` error codes and the `xc_source` byte-source interface.
- ADR 0007 and the credentials file format (docs/formats/credentials.md): logins for sources that need them, stored as host-scoped tokens outside the map, sent only over HTTPS and re-checked on every redirect (design; implemented in M3–M6).
- libFuzzer target for the XDVDFS reader, with seeds generated from the fixtures. Every build replays the seeds; a `fuzz` preset and CI job run the fuzzer with ASan and UBSan.
- Extractor (`xc_extract`) and the `xc_sink` interface: directories first, then files in offset order through one caller-provided buffer, coalescing small gaps into a single read span (ADR 0004); progress reporting and cancellation. Output matches `extract-xiso -x` byte for byte.
- Host platform layer (`platform/host`): file source and directory sink, with UTF-8 and long-path support on Windows.
- `xcat-cli list` and `xcat-cli extract` for PCs.
- FATX probe XBE (`app/fatx-probe`): measures which names, paths and directory sizes the console's FATX driver accepts, so the M2 naming rules come from the real kernel.
- FATX rules measured on the console (docs/formats/fatx-names.md) and `xcat/fatx.h`: games whose files the Xbox hard disk can't store are refused, with every problem listed (ADR 0008); install folder names are generated from titles. `xcat-cli check` runs the same check on a PC.
- On-box install (M2): the XBE installs the images in `E:\xcat\isos\` to `E:\Games\<title>` with FATX and free-space checks, then reads every file back to verify it. A synthetic test image is bundled. Xbox platform layer (`platform/nxdk`): file source, FATX directory sink, free space, drive mounting.
- ADR 0009 (accepted): install from zip archives by streaming the zip with HTTP `Range` (Zip64, stored + Deflate, CRC-32 verified), based on archive.org tests showing that "file inside a zip" URLs can't be resumed and stop at ~1 GB.
- HTTP client profile (docs/formats/http.md): the strict HTTP/1.1 subset the console speaks, with limits, status handling, retries, resume and per-hop credentials.
- URL parser (`xcat/url.h`) and HTTP response parser (`xcat/http.h`): `http`/`https` URLs with percent-encoding and redirect resolution; response heads, `Content-Range` and chunked bodies, rejecting anything ambiguous. Both are fuzzed in CI.
- Stream-order report (`xc_xdvdfs_stream_order`, `xcat-cli layout`): shows whether every directory table precedes the files it describes, i.e. whether an image can be installed from a sequential (Deflate) stream in one pass (ADR 0009 open question).
- HTTP client (`xcat/http_client.h`) over the new `xc_net`/`xc_clock` platform interfaces (`xcat/net.h`): `Range` requests for one remote file, up to 5 redirects with per-hop credentials, keep-alive, timeouts, retries with jittered backoff and `Retry-After`, and resume after a dropped connection without delivering any byte twice. Detects servers that ignore `Range` and files that change mid-download. `xc_http_source` exposes a remote file as an `xc_source`. New error codes for network and HTTP failures.
- `xcat-cli get <url> <file>`: downloads a file over plain HTTP with the core client, on host sockets (`xc_host_net`, Winsock or BSD) and a monotonic clock (`xc_host_clock`). Shows retries, progress and MB/s; `--timeout` and `--attempts` tune the retry behaviour.
- Range test server (tools/testserver/rangeserver.py) with scripted faults: redirects, error statuses with `Retry-After`, dropped and stalled connections, `200` instead of `206`, chunked bodies, a file that changes mid-download. 24 end-to-end `get_*` tests run `xcat-cli get` against it in CI.
- Network test XBE (`app/net-test`, M3): downloads a test file from a PC over the LAN with the core HTTP client and reports progress, every retry (e.g. while the cable is pulled), average MB/s and the CRC-32, on screen and appended to `E:\xcat\net-test.log`. It resumes after a lost connection; a power cut starts it over (journaled resume is M4). Xbox networking (`xc_nx_net`, lwIP) and clock (`xc_nx_clock`) in `platform/nxdk`. `tools/testserver/make_test_file.py` makes the deterministic test file and prints its size and CRC-32.
- CRC-32 (`xcat/crc32.h`), the zip/zlib variant. `xcat-cli get` now prints the CRC-32 of the download.

### Changed

- Roadmap: zip support moves from optional M9b to M5b, right after HTTPS (ADR 0009 accepted).
- Unity test framework updated to v2.7.0.
