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
