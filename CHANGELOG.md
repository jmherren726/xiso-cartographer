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
