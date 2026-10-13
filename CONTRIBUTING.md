# Contributing

Thanks for helping. Read [docs/HANDOFF.md](docs/HANDOFF.md) first: it explains what the project is, how it is structured and the coding standards (§7).

## Ground rules

- **No copyrighted content.** Never add game images, download links, ROMs, BIOS/MCPX dumps, XDK material or secrets to the repo, issues, fixtures or CI. Test data is generated from dummy content by `tools/fixtures/`.
- **No bypasses.** Code that gets past captchas, bot protection or access controls, or that violates a source's terms, will not be merged.
- **nxdk only.** The leaked Microsoft XDK is never used.

## Setup

Clone with submodules (`git clone --recursive`, or `git submodule update --init` in an existing clone).

### Host build (core library and tests)

Prerequisites: CMake 3.25+, Ninja, Python 3.10+ (generates the test fixtures at build time), and clang-format/clang-tidy from LLVM, plus a C11 compiler:

- **Linux/macOS:** gcc or clang.
- **Windows:** Visual Studio 2022 Build Tools with the "Desktop development with C++" workload. It provides `cl`, the Windows SDK and the C runtime, which clang-cl also needs. Run the commands from a *Developer PowerShell for VS 2022*. To use clang-cl instead of cl, set `CC=clang-cl` before configuring.

```sh
cmake --preset host-debug          # Debug, warnings as errors, ASan (+ UBSan on gcc/clang)
cmake --build --preset host-debug
ctest --preset host-debug
```

`host-release` is the same build without sanitizers. Build output goes to `build/<preset>/` (git-ignored).

| CMake option | Default | Effect |
|---|---|---|
| `XCAT_BUILD_TESTS` | `ON` | Build the unit tests |
| `XCAT_WERROR` | `ON` | Treat warnings as errors |
| `XCAT_SANITIZE` | `OFF` (`ON` in `host-debug`) | ASan, plus UBSan on non-Windows toolchains |
| `XCAT_LARGE_FIXTURES` | `OFF` | Also generate and test the >4 GiB sparse fixture (needs a filesystem with sparse files) |

### Formatting and static analysis

- Format changed files with `clang-format -i <files>`. The style is in [.clang-format](.clang-format).
- Run clang-tidy against the compile database: `clang-tidy -p build/host-debug <files>`. The checks are in [.clang-tidy](.clang-tidy).
- CI checks both on every pull request with pinned versions: clang-format **23.1.3** and clang-tidy **22.1.8**, installed from PyPI. Use the same clang-format version locally (`pip install clang-format==23.1.3`), because other versions can format differently.

### Continuous integration

[`.github/workflows/ci.yml`](.github/workflows/ci.yml) runs on every pull request and on pushes to `main`:

| Job | What it checks |
|---|---|
| `format` | `clang-format --dry-run --Werror` on all tracked `.c`/`.h` files |
| `clang-tidy` | `.clang-tidy` profile against the `host-debug` compile database |
| `linux (gcc\|clang, host-debug\|host-release)` | Build + unit tests on Ubuntu 24.04 (ASan + UBSan in debug) |
| `windows (cl\|clang-cl, host-debug\|host-release)` | Build + unit tests on Windows Server 2025 with the runner's Visual Studio (VS 2026, MSVC 19.51, LLVM clang-cl 20.1 as of 2026-10; ASan in debug) |
| `xbe` | Builds the nxdk Docker image and the XBE; uploads `default.xbe` + ISO as an artifact |
| `fixtures` | Generator unit tests; builds all fixtures (incl. >4 GiB), cross-checks them with extract-xiso, and checks the output is deterministic |
| `fuzz` | Builds the libFuzzer targets (clang, ASan + UBSan), replays the seeds, then fuzzes each parser for 60 s; crashing inputs are uploaded as an artifact |

All of these jobs are required checks in the `main` ruleset. If you rename a job or change the matrix, update the ruleset's required checks in the same MR, or pull requests will wait forever for a check that no longer exists.

### Source layout for host code

- `core/` is libxcat: portable, with no OS calls.
- `platform/host/` adapts it to a PC: a stdio file source and a directory sink. Use these in tests and tools instead of writing new file I/O.
- `tools/xcat-cli/` is the command-line front end.

Fixture extraction is tested end to end: one `extract_<fixture>` CTest per fixture runs `xcat-cli extract` and compares the output with the manifest.

### Adding a unit test

Tests use [Unity](https://github.com/ThrowTheSwitch/Unity) ([ADR 0003](docs/adr/0003-unit-test-framework.md)). Create `core/tests/test_<module>.c` with `setUp`/`tearDown` and a `main()` that calls `RUN_TEST` for each test, then add `xcat_add_test(test_<module>)` to [core/tests/CMakeLists.txt](core/tests/CMakeLists.txt). [test_version.c](core/tests/test_version.c) is the minimal example.

### Fuzzing

Every parser of external input has a libFuzzer target next to its tests (HANDOFF §7.3). Today that's `core/tests/fuzz_xdvdfs.c`; its header comment describes the input layout.

- **Every build** compiles each target as `<name>_replay` and CTest runs it over the seeds (`fuzz_<name>_seeds`). This keeps the harness compiling on every compiler and turns past crashes into regression tests.
- **Real fuzzing** needs clang on Linux or macOS (on Ubuntu, also `libclang-rt-18-dev` or your clang version's equivalent):

  ```sh
  cmake --preset fuzz && cmake --build --preset fuzz
  mkdir -p build/fuzz/corpus
  ./build/fuzz/core/tests/fuzz_xdvdfs -max_len=65536 build/fuzz/corpus build/fuzz/fuzz-seeds
  ```

- **Seeds** are made from the fixtures by `tools/fixtures/fuzz_seeds.py` at build time.
- **When the fuzzer finds a crash**, fix the bug and add a unit test that reproduces it in the same MR. If the input is a useful regression case, commit it under `core/tests/fuzz-regressions/<target>/` and add it to the replay test's seeds.
- **New parsers** (`http`, `map`, `catalog`, JSON, `credentials`) come with a fuzz target and an `xcat_add_fuzzer()` line in the MR that adds the parser.

### Test fixtures

Core tests use synthetic xiso images generated from dummy data by [tools/fixtures/](tools/fixtures/README.md). Generating them needs Python 3.10+ and no packages: `make fixtures`, or `python3 tools/fixtures/fixtures.py --out build/fixtures` without make. Never add real game images to the repo or to tests.

### Xbox build

Built with nxdk inside Docker: `make docker-xbe`. See [docs/build-xbox.md](docs/build-xbox.md) for prerequisites, the Windows line-ending fix for submodules, and how to run the XBE in xemu and on hardware. Platform-affecting MRs record their xemu/hardware result in [docs/testing.md](docs/testing.md).

## Merge requests

"MR" means a merge request, called a pull request on GitHub.

- **Small and focused:** one concern per MR, ideally under ~400 changed lines (generated files excluded). Split refactors from behaviour changes.
- **Branch names:** `<type>/<short-description>`, e.g. `feat/xdvdfs-reader`, `fix/http-redirect-loop`.
- **Titles:** [Conventional Commits](https://www.conventionalcommits.org/), e.g. `feat(core): …`, `fix(net): …`, `docs: …`, `chore(ci): …`. MRs are squash-merged with the MR title as the commit message.
- **Docs in the same MR.** If an MR changes behaviour, a format, a build step, a public header or a user-visible message, update the matching doc in that MR (README, `docs/ARCHITECTURE.md`, `docs/formats/*`, `docs/testing.md`, `CLAUDE.md`). Reviewers reject MRs that leave docs stale.
- **CHANGELOG entry** under "Unreleased" in [CHANGELOG.md](CHANGELOG.md) for every user-visible change, following [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
- **Tests in the same MR.** New logic comes with unit tests. Bug fixes come with a test that failed before the fix. Platform changes record the xemu/hardware result in the MR description.
- **CI must be green.** Never disable or skip a test to merge. A flaky test is a bug: fix it, or open an issue that blocks the next release.
- **Decisions get an ADR.** Any choice that is hard to reverse (dependency, format, protocol, licence) goes in `docs/adr/NNNN-title.md`. See [ADR 0001](docs/adr/0001-record-architecture-decisions.md).

### Review checklist

The [MR template](.github/pull_request_template.md) contains this list:

- [ ] Tests added or updated, CI green
- [ ] Docs updated, or "no doc impact" stated
- [ ] CHANGELOG updated
- [ ] No copyrighted content, links, ROMs or secrets
- [ ] Format/version changes are backward compatible or bump the version
- [ ] Hardware/xemu test recorded (if platform-affecting)

## Versioning

SemVer tags `vX.Y.Z`. Before 1.0, minor bumps may break things. The map, catalog and journal formats each carry their own version number, documented in `docs/formats/`.

## Third-party code

Third-party code comes in as pinned git submodules under `third_party/`, or vendored with its licence file intact, and is listed in `THIRD_PARTY.md`. Upgrades go in a dedicated MR.

## Licence

By contributing, you agree that your contributions are licensed under the [MIT License](LICENSE). Don't copy in code under GPL or other copyleft licences. See [ADR 0002](docs/adr/0002-license.md).
