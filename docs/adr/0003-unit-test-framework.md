# 0003. Use Unity for host unit tests

- **Status:** Accepted
- **Date:** 2026-10-13

## Context

`core/` is portable C11 and needs unit tests that build and run on Linux (gcc, clang) and Windows (MSVC, clang-cl), with ASan/UBSan, warnings as errors, and CTest integration. Most tests will exercise binary parsers (XDVDFS, HTTP, map, catalog), so byte-array and 64-bit assertions matter. Platform services are injected as function-pointer interfaces (HANDOFF §7.4), so tests can use hand-written fakes and don't need a mocking framework.

The handoff named three candidates:

| | Unity | greatest | cmocka |
|---|---|---|---|
| Licence | MIT | ISC | Apache-2.0 |
| Integration | One `.c` + headers | Single header | Library build (CMake) |
| Byte-array / hex / 64-bit asserts | Yes (`TEST_ASSERT_EQUAL_HEX8_ARRAY`, `UNITY_SUPPORT_64`) | Basic (`ASSERT_MEM_EQ`) | Basic (`assert_memory_equal`) |
| Mocking | Optional (CMock, Ruby) | No | Built in |
| MSVC support | Yes | Yes | Yes, needs more build setup |
| Maturity | Widely used in embedded C | Small, stable | Widely used |

## Decision

Use **Unity**, pinned as a git submodule at `third_party/unity` (tag `v2.6.1`). It is built from source by `core/tests/CMakeLists.txt`, with `UNITY_SUPPORT_64` always defined. CMock is not used. Fakes for `xc_io`, `xc_net` and `xc_clock` are written by hand.

Each test file is a standalone executable registered with CTest through `xcat_add_test()`. Fuzz targets (libFuzzer) are separate and don't use Unity.

## Consequences

- Tests read naturally for binary formats (`TEST_ASSERT_EQUAL_HEX8_ARRAY`, `TEST_ASSERT_EQUAL_UINT64`).
- The MIT licence matches the project ([ADR 0002](0002-license.md)). Unity is test-only and never ships in the XBE.
- Each test file needs a small `main()` that calls `RUN_TEST` for each test. We accept that boilerplate instead of adding Unity's Ruby runner generator.
- Contributors must clone with `--recursive` or run `git submodule update --init`.
- Unity upgrades arrive through Dependabot submodule PRs and are merged as dedicated MRs.
