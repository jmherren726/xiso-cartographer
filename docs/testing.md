# Testing

## Test ladder

Every change climbs as far up this ladder as it affects (see [HANDOFF.md §4](HANDOFF.md#4-strategies)):

1. **Host unit tests:** `ctest --preset host-debug`. Run by CI on Linux and Windows for every pull request.
   - Parser and extractor tests use the synthetic xiso fixtures from [tools/fixtures/](../tools/fixtures/README.md) (small, deep, XGD1/XGD2/XGD3 full-disc, and >4 GiB with `-DXCAT_LARGE_FIXTURES=ON`; CMake generates them at build time). CI checks the fixtures themselves against extract-xiso.
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
| 2026-10-13 | M0 | `e4c26ab` (local Docker build) | Hardware: main console (see bench) | ✅ Pass | `app/bin/default.xbe` copied with FileZilla (FTP) and launched from the dashboard. Same three lines as in xemu. |
| 2026-10-13 | M0 | `e4c26ab` (local Docker build) | xemu v0.8.136 (Windows 11) | ✅ Pass | `app/xiso-cartographer.iso` via Load Disc. Shows `xiso-cartographer 0.0.0`, `libxcat version number: 0x000000`, `Hello from nxdk. Toolchain OK.` |

## Benchmarks

| Date | Measurement | Setup | Result |
|---|---|---|---|
| _pending_ | PC → archive.org download speed (HANDOFF §8.1) | `curl -r 0-104857599` against a storage server | |
