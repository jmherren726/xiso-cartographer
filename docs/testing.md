# Testing

## Test ladder

Every change climbs as far up this ladder as it affects (see [HANDOFF.md §4](HANDOFF.md#4-strategies)):

1. **Host unit tests:** `ctest --preset host-debug`. Run by CI on Linux and Windows for every pull request.
   - Parser and extractor tests use the synthetic xiso fixtures from [tools/fixtures/](../tools/fixtures/README.md) (small, deep, XGD1/XGD2/XGD3 full-disc, and >4 GiB with `-DXCAT_LARGE_FIXTURES=ON`; CMake generates them at build time). CI checks the fixtures themselves against extract-xiso.
   - Every parser also has a libFuzzer target. CTest replays its seeds on every build, and CI fuzzes each parser for 60 s per run ([CONTRIBUTING.md#fuzzing](../CONTRIBUTING.md#fuzzing)).
2. **Host integration tests:** `xcat-cli` against the fake HTTP/HTTPS server and fixture images. Arrives with M1/M3.
3. **xemu:** manual for now. Needs your own BIOS/MCPX/HDD files, which can't go in CI. See [build-xbox.md](build-xbox.md#run-in-xemu).
4. **Real hardware:** manual. See [build-xbox.md](build-xbox.md#run-on-hardware).

Steps 3 and 4 are required for every milestone that touches the platform layer. Record each run in the results log below and in the MR description.

## Hardware test bench

| Console | Revision | RAM | HDD | Mod | Dashboard |
|---|---|---|---|---|---|
| Main console | _revision?_ | _64/128 MB?_ | _stock/upgraded?_ | _soft/hard?_ | _dashboard?_ |

## Results log

Newest first. "Build" is the commit or CI artifact tested.

| Date | Milestone | Build | Where | Result | Notes |
|---|---|---|---|---|---|
| 2026-10-14 | M2 (step 3) | `main` after #25 (local Docker build) | Hardware: main console (see bench) | ✅ Pass | One of the owner's retail games, full-disc dump trimmed with `extract-xiso -r` to a 3,112,328,144-byte xiso, copied by FTP to `E:\xcat\isos\` (~11.7 MB/s), installed: **21 files in 211,952 ms (~14,340 KB/s, ~14.7 MB/s)**. Read-back verify: all 21 files match. **The installed game launches and plays from the dashboard.** M2 exit test met. |
| 2026-10-14 | M2 (step 2) | `main` after #25 (local Docker build) | Hardware: main console (see bench) | ✅ Pass | `app/bin/` (XBE + `samples/`) copied by FTP. Bundled `small.iso` installed to `E:\Games\small`: 9 files in 19 ms (10,141 KB/s; tiny image, so not a throughput figure). Read-back verify: all 9 files match. |
| 2026-10-14 | M2 (step 1) | `feat/onbox-extract` (local Docker build) | xemu v0.8.136, kernel 1.0.4627.1 | ✅ Pass | Bundled `small.iso` copied to `E:\xcat\isos\`, installed to `E:\Games\small`: 9 files, 3 folders, 192 KB in 62 ms (3,107 KB/s, emulated disk). Read-back verify: all 9 files match the image. "Done: 1 of 1 image(s) installed and verified." |
| 2026-10-13 | M1 | `feat/extractor` | Host: Windows (cl, clang-cl, clang), Ubuntu 24.04 (gcc, clang) | ✅ Pass | `xcat-cli extract` output matches each fixture's manifest (all layouts, incl. >4 GiB on Linux). Direct check: `diff -r` against `extract-xiso -x` output is identical for `small` (9 files) and `deep` (311 files, 17 dirs). Parser fuzzed (#21). |
| 2026-10-13 | M0 | `e4c26ab` (local Docker build) | Hardware: main console (see bench) | ✅ Pass | `app/bin/default.xbe` copied with FileZilla (FTP) and launched from the dashboard. Same three lines as in xemu. |
| 2026-10-13 | M0 | `e4c26ab` (local Docker build) | xemu v0.8.136 (Windows 11) | ✅ Pass | `app/xiso-cartographer.iso` via Load Disc. Shows `xiso-cartographer 0.0.0`, `libxcat version number: 0x000000`, `Hello from nxdk. Toolchain OK.` |

## M2: on-box install from the hard disk

**Exit test:** an image already on the HDD installs to `E:\Games\<title>` in xemu and on hardware, and every installed file matches the image (checked on the console itself; see [build-xbox.md](build-xbox.md#what-the-app-does-m2)).

| Step | Image | Where | How |
|---|---|---|---|
| 1 | Bundled `small.iso` (synthetic) | xemu | Load Disc → `app/xiso-cartographer.iso`; the app copies the sample to `E:\xcat\isos\` and installs it |
| 2 | Bundled `small.iso` | Hardware | FTP `app/bin/` (XBE + `samples/`) to `E:\Apps\xiso-cartographer\` and launch |
| 3 | One of your own games as a trimmed xiso under 4 GiB | Hardware | FTP it to `E:\xcat\isos\`, launch; then launch the installed game from the dashboard |

A run passes when the screen ends with "Done: N of N image(s) installed and verified." Record the summary lines (files, KB/s) in the results log above.

## FATX probe (M2)

No authoritative list of FATX naming rules exists, and a game's files can't be renamed on install, because the game opens them by name. So the rules in `core/src/fatx.c` ([fatx-names.md](formats/fatx-names.md)) come from the console's own kernel, measured by `app/fatx-probe`.

**What it tests**, under `E:\xcat-fatx-probe` (deleted before and after the run):

| Test | What |
|---|---|
| Name length | Files and directories with 40–43-character names |
| Characters | Every byte 0x01–0xFF (except `\`) inside a file name |
| Edge cases | Trailing dot, trailing space, leading space, `...`, `CON`, … |
| Case | Is lookup case-insensitive; can `CASEPROBE` be created next to `CaseProbe`? |
| Path length | Longest directory path and file path, excluding the drive |
| Entries | Files in one directory, up to 5,000 |

**How to run it:**

1. `make docker-xbe` (or download the CI artifact) builds `app/fatx-probe/xcat-fatx-probe.iso` and `app/fatx-probe/bin/default.xbe`.
2. **xemu:** Machine → Load Disc → the ISO. It needs an HDD image with a formatted E: partition. **Hardware:** FTP `default.xbe` to e.g. `E:\Apps\xcat-fatx-probe\` and launch it.
3. Wait for "Done" (the entries test takes the longest), and note or photograph the summary.
4. Fetch `E:\xcat-fatx-probe.txt` over FTP (hardware). In xemu, the on-screen summary is enough unless something looks unexpected.

### Probe results

| Date | Where | Kernel | Name length (file/dir) | Rejected bytes | Case | Longest path (dir/file) | Entries | Report |
|---|---|---|---|---|---|---|---|---|
| 2026-10-13 | xemu v0.8.136 | 1.0.4627.1 | ≤ 42 / ≤ 42 (43 rejected) | 83 of 254: see below | lookup insensitive; other-case duplicate rejected | dir 250; file: see note | 4,096 (then rejected) | on-screen summary (probe v2, `ddc3b53`) |
| 2026-10-13 | Hardware (main console) | not recorded | same as xemu | same as xemu | same as xemu | same as xemu | same as xemu | reported by the owner as identical to the xemu run; report file not captured |

**Rejected bytes (xemu run):**

- `0x01`–`0x1F` (all control characters). `0x00` can't occur and `0x5C` (`\`) is the separator, so neither was tested.
- ASCII: `0x22 "`, `0x2A *`, `0x2B +`, `0x2C ,`, `0x2F /`, `0x3A :`, `0x3B ;`, `0x3C <`, `0x3D =`, `0x3E >`, `0x3F ?`, `0x7C |`. All other printable ASCII is accepted, including space, `[` `]` and `0x7F`.
- Bytes ≥ `0x80` (40, no obvious pattern): `84 86 87 89 8B 8F 91 9F A0 A1 A2 A5 A6 A9 AA AD AE B0 BF C0 C1 C2 C5 C6 C9 CA CD CE D0 DF E0 E1 E4 E5 E6 E7 E9 EF F1 FF`.

**Edge names:** trailing dot, trailing space, leading space, `a..b`, `...`, `CON` and `nul.txt` are all accepted.

**Path note:** paths are counted from the first `\` after `E:`. Directories nested to exactly 250 characters were created, and 251 was rejected. Every file the probe tried went inside the 250-character directory, so it was over the limit. A file path of exactly 250 wasn't tried directly. The limit is evidently on the whole path, and the hardware run should confirm a 250-character file path.

**Consequences for M2:** FATX's 250-character path limit is stricter than nxdk's winapi limit (259 including `E:`), so the app can use nxdk's file calls if it enforces 250 itself. A game is installable only if every path under `\Games\<title>\` fits in 250 characters, every name is valid, and no directory has more than 4,096 entries.

## Benchmarks

| Date | Measurement | Setup | Result |
|---|---|---|---|
| _pending_ | PC → archive.org download speed (HANDOFF §8.1) | `curl -r 0-104857599` against a storage server | |
