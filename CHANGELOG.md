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
- ADR 0009 (proposed): install from zip archives by streaming the zip with HTTP `Range` (Zip64, stored + Deflate, CRC-32 verified), based on archive.org tests showing that "file inside a zip" URLs can't be resumed and stop at ~1 GB.
- Stream-order report (`xc_xdvdfs_stream_order`, `xcat-cli layout`): shows whether every directory table precedes the files it describes, i.e. whether an image can be installed from a sequential (Deflate) stream in one pass (ADR 0009 open question).

### Changed

- Unity test framework updated to v2.7.0.
