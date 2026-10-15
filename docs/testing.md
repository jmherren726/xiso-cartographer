# Testing

## Test ladder

Every change climbs as far up this ladder as it affects (see [HANDOFF.md §4](HANDOFF.md#4-strategies)):

1. **Host unit tests:** `ctest --preset host-debug`. Run by CI on Linux and Windows for every pull request.
   - Parser and extractor tests use the synthetic xiso fixtures from [tools/fixtures/](../tools/fixtures/README.md) (small, deep, XGD1/XGD2/XGD3 full-disc, and >4 GiB with `-DXCAT_LARGE_FIXTURES=ON`; CMake generates them at build time). CI checks the fixtures themselves against extract-xiso.
   - Every parser also has a libFuzzer target. CTest replays its seeds on every build, and CI fuzzes each parser for 60 s per run ([CONTRIBUTING.md#fuzzing](../CONTRIBUTING.md#fuzzing)).
2. **Host integration tests:** `xcat-cli` against the fixture images (`extract_*`, `check_*`, `layout_*`) and against the Range test server with scripted faults (`get_*`, `install_*` including installs killed mid-way and resumed, `map_cli`; [tools/testserver/](../tools/testserver/README.md)). Also run by CTest; HTTPS joins in M5.
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
| 2026-10-15 | M3 (step 3) | #35 CI artifact (`e6b5977`) | Hardware: main console (see bench), same LAN and PC | ✅ Pass | Same 1 GiB file. Network cable pulled at ~31 s (~340 MiB) and plugged back in: no data for 15 s, then "The server stopped responding, retrying in 0.9 s", and the download **resumed at ~347 MiB, not 0**. Finished in 108 s (9.9 MB/s average including the outage), 2 retries, longest pause 16.3 s, **CRC-32 `6775e853` matches**. The cable was back in before the first retry, so the reconnect worked on the first try; a longer outage (several failed connects with backoff) wasn't exercised. **M3 exit test met** (1 GB over the LAN, CRC match, MB/s recorded, survives a cable pull). Side note: in 2 of the 4 runs today, the very first request after startup timed out once (15 s) before working on the retry; worth a look before M4. |
| 2026-10-15 | M3 (step 2) | #35 CI artifact (`c208e95`) | Hardware: main console (see bench), LAN to a Windows PC running `rangeserver.py` | ✅ Pass | 1 GiB test file (`make_test_file.py --size 1G`) downloaded twice: **1,073,741,824 bytes in 90 s, 11.8 MB/s average** (steady 11.8 MB/s throughout), 0 retries, CRC-32 `6775e853` matches both times. About the same as the console's FTP upload (~11.7 MB/s), so the HTTP client isn't the bottleneck. A power cut mid-download restarted the test from 0 on the next boot, as expected: the test keeps progress in memory only (resume across power cuts is M4). |
| 2026-10-14 | M2 (step 3) | `main` after #25 (local Docker build) | Hardware: main console (see bench) | ✅ Pass | One of the owner's retail games, full-disc dump trimmed with `extract-xiso -r` to a 3,112,328,144-byte xiso, copied by FTP to `E:\xcat\isos\` (~11.7 MB/s), installed: **21 files in 211,952 ms (~14,340 KB/s, ~14.7 MB/s)**. Read-back verify: all 21 files match. **The installed game launches and plays from the dashboard.** M2 exit test met. |
| 2026-10-14 | M2 (step 2) | `main` after #25 (local Docker build) | Hardware: main console (see bench) | ✅ Pass | `app/bin/` (XBE + `samples/`) copied by FTP. Bundled `small.iso` installed to `E:\Games\small`: 9 files in 19 ms (10,141 KB/s; tiny image, so not a throughput figure). Read-back verify: all 9 files match. |
| 2026-10-14 | M2 (step 1) | `feat/onbox-extract` (local Docker build) | xemu v0.8.136, kernel 1.0.4627.1 | ✅ Pass | Bundled `small.iso` copied to `E:\xcat\isos\`, installed to `E:\Games\small`: 9 files, 3 folders, 192 KB in 62 ms (3,107 KB/s, emulated disk). Read-back verify: all 9 files match the image. "Done: 1 of 1 image(s) installed and verified." |
| 2026-10-13 | M1 | `feat/extractor` | Host: Windows (cl, clang-cl, clang), Ubuntu 24.04 (gcc, clang) | ✅ Pass | `xcat-cli extract` output matches each fixture's manifest (all layouts, incl. >4 GiB on Linux). Direct check: `diff -r` against `extract-xiso -x` output is identical for `small` (9 files) and `deep` (311 files, 17 dirs). Parser fuzzed (#21). |
| 2026-10-13 | M0 | `e4c26ab` (local Docker build) | Hardware: main console (see bench) | ✅ Pass | `app/bin/default.xbe` copied with FileZilla (FTP) and launched from the dashboard. Same three lines as in xemu. |
| 2026-10-13 | M0 | `e4c26ab` (local Docker build) | xemu v0.8.136 (Windows 11) | ✅ Pass | `app/xiso-cartographer.iso` via Load Disc. Shows `xiso-cartographer 0.0.0`, `libxcat version number: 0x000000`, `Hello from nxdk. Toolchain OK.` |

## M2: on-box install from the hard disk

**Exit test:** an image already on the HDD installs to `E:\Games\<title>` in xemu and on hardware, and every installed file matches the image (checked on the console itself; see [build-xbox.md](build-xbox.md#what-the-app-does-m4)).

| Step | Image | Where | How |
|---|---|---|---|
| 1 | Bundled `small.iso` (synthetic) | xemu | Load Disc → `app/xiso-cartographer.iso`; the app copies the sample to `E:\xcat\isos\` and installs it |
| 2 | Bundled `small.iso` | Hardware | FTP `app/bin/` (XBE + `samples/`) to `E:\Apps\xiso-cartographer\` and launch |
| 3 | One of your own games as a trimmed xiso under 4 GiB | Hardware | FTP it to `E:\xcat\isos\`, launch; then launch the installed game from the dashboard |

A run passes when the screen ends with "Done: N of N image(s) installed and verified." Record the summary lines (files, KB/s) in the results log above.

## M3: network download test

**Exit test** ([HANDOFF.md §5](HANDOFF.md)): a 1 GB download over the LAN whose CRC-32 matches, with MB/s recorded, that survives a pulled cable. `app/net-test` downloads one file with the core HTTP client (the same code `xcat-cli get` uses) over the console's own network stack (lwIP, `platform/nxdk/src/nx_net.c`), and reports progress, every retry, the average MB/s and the CRC-32.

**On the PC:**

1. Make the test file (pseudo-random test data, never a game): `python3 tools/testserver/make_test_file.py --out C:\xcat-net --size 1G`. It prints `url=`, `size=` and `crc32=` lines.
2. Serve it to the LAN: `python3 tools/testserver/rangeserver.py --root C:\xcat-net --bind 0.0.0.0`. Allow Python through the Windows firewall (private networks) when asked. Optional check from the PC: `xcat-cli get http://127.0.0.1:8080/xcat-test-1073741824.bin <file>` prints the same CRC-32.
3. Write `net-test.txt` from the printed lines, with `<pc-ip>` replaced by the PC's LAN address (`ipconfig`):

   ```
   url=http://192.168.1.10:8080/xcat-test-1073741824.bin
   size=1073741824
   crc32=<printed by make_test_file.py>
   ```

   Optional lines: `save=yes` also writes the file to `E:\xcat\net-test.bin` (network and disk together, like a real install; delete the file afterwards), and `attempts=N` (default 40, about half an hour of outage) sets how many consecutive failures end the test.

**On the console:**

| Step | Where | How |
|---|---|---|
| 1 | xemu (optional) | Enable networking (**Settings → Network**, NAT). The PC is then usually `10.0.2.2`. Put `net-test.txt` on the E: partition of the HDD image, then **Load Disc →** `app/net-test/xcat-net-test.iso`. A small `--size 100M` file is enough here. |
| 2 | Hardware | FTP `net-test.txt` to `E:\xcat\` and `app/net-test/bin/default.xbe` to e.g. `E:\Apps\xcat-net-test\`. Launch it. Let it run to the end once without touching anything. |
| 3 | Hardware | Launch it again. After about 30 s, **pull the network cable** for 30–60 s, then plug it back in. The screen should show retries ("The server stopped responding" or "network connection failed"), then progress resuming from where it stopped, never from 0. |
| 4 | Hardware (optional) | Once more with `save=yes`, for the speed with the disk writes included. |

The cable pull tests a lost connection within one run. A power cut or reboot can't be resumed by this test: it keeps its progress in memory only, so the next launch starts from 0. Resuming an install after a power cut is M4's exit test (a journal on disk).

A run passes when the screen ends with "CRC-32 … matches, PASS". Each run is appended to `E:\xcat\net-test.log` (it starts over past 1 MiB); fetch it over FTP and record in the results log: the average MB/s, the retries and the longest pause (step 3), and the disk-write line (step 4). The MB/s figure also sizes the install buffers ([ARCHITECTURE.md §6](ARCHITECTURE.md)).

## M4: streamed install over HTTP

**Exit test** ([HANDOFF.md §4.1](HANDOFF.md#41-roadmap)): an over-4 GB fixture installs over the LAN, and resumes after a power cut. The main app (`app/bin/default.xbe`) installs every entry of `E:\xcat\map.txt` while it streams in, with a journal in the install folder ([journal.md](formats/journal.md)), then reads every file back and compares its CRC-32.

**On the PC:**

1. Make the fixtures, including the 4.3 GB `large` image. It's sparse, so it takes almost no disk space, and it holds test data only, never a game: `python3 tools/fixtures/fixtures.py --out C:\xcat-fixtures --large`.
2. Serve them to the LAN: `python3 tools/testserver/rangeserver.py --root C:\xcat-fixtures --bind 0.0.0.0`. Optional check from the PC: `xcat-cli install http://127.0.0.1:8080/small.iso <some folder>` ends with "verified".
3. Write `map.txt` with the PC's LAN address:

   ```
   # xcat-map v1
   00000001 = http://192.168.1.10:8080/large.iso
   ```

   (Any 8 hex digits work as the key until the catalog exists. The install folder is named after the file: `E:\Games\large`.)

**On the console:**

| Step | Where | How |
|---|---|---|
| 1 | Hardware | FTP `map.txt` to `E:\xcat\` and `app/bin/` to e.g. `E:\Apps\xiso-cartographer\`. Empty `E:\xcat\isos\` first, or the M2 test image installs again too. About 4.4 GB must be free on E:. |
| 2 | Hardware | Launch it. The screen shows "writing files" with MiB, file count and MB/s every 5 s. At roughly a third or half way, **switch the console off at the power button**. |
| 3 | Hardware | Switch it on and launch the app again. The progress must pick up near where it stopped (at most 64 MiB earlier), not at 0. It ends with "OK: … verified (resumed)", the line "1 file(s) continued from a checkpoint (… MiB were already there)", and "Done: 1 of 1". |
| 4 | Hardware (optional) | Launch once more: the game goes to `E:\Games\large (2)`, because the first one is finished ("already installed there"). Delete that folder afterwards. |

A pass is "OK: … verified (resumed)" in step 3. Send `E:\xcat\install.log`: it has every run, the MB/s, and for any retry the time the last connection's lookup and handshake took (that's to pin down the one-off 15 s first-request delay from M3). Delete `E:\Games\large` afterwards; it's test data.

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
| File size (v4) | The largest file FATX stores: 4 GiB minus 2 clusters, minus 1 cluster, minus 1 cluster + 1 byte, minus 2 KiB, minus 1 byte. Each is set directly and also grown across the end by writing its last 1 MiB. Needs about 4.1 GB free on E: (skipped otherwise). Added after a 4,294,965,248-byte file failed at its end in the M4 test. |

**How to run it:**

1. `make docker-xbe` (or download the CI artifact) builds `app/fatx-probe/xcat-fatx-probe.iso` and `app/fatx-probe/bin/default.xbe`.
2. **xemu:** Machine → Load Disc → the ISO. It needs an HDD image with a formatted E: partition. **Hardware:** FTP `default.xbe` to e.g. `E:\Apps\xcat-fatx-probe\` and launch it.
3. Wait for "Done" (the entries and file-size tests take the longest), and note or photograph the summary.
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
