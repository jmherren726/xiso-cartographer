# Testing

## Test ladder

Every change climbs as far up this ladder as it affects (see [HANDOFF.md §4](HANDOFF.md#4-strategies)):

1. **Host unit tests:** `ctest --preset host-debug`. Run by CI on Linux and Windows for every pull request.
2. **Host integration tests:** `xcat-cli` against the fake HTTP/HTTPS server and fixture images. Arrives with M1/M3.
3. **xemu:** manual for now. Needs your own BIOS/MCPX/HDD files, which can't go in CI. See [build-xbox.md](build-xbox.md#run-in-xemu).
4. **Real hardware:** manual. See [build-xbox.md](build-xbox.md#run-on-hardware).

Steps 3 and 4 are required for every milestone that touches the platform layer. Record each run in the results log below and in the MR description.

## Hardware test bench

| Console | Revision | RAM | HDD | Mod | Dashboard |
|---|---|---|---|---|---|
| _to fill in_ | | | | | |

## Results log

Newest first. "Build" is the commit or CI artifact tested.

| Date | Milestone | Build | Where | Result | Notes |
|---|---|---|---|---|---|
| _pending_ | M0 | MR-4 | xemu | | Hello XBE from `app/xiso-cartographer.iso` |
| _pending_ | M0 | MR-4 | Hardware | | `default.xbe` via FTP to `E:\Apps\xiso-cartographer\` |

## Benchmarks

| Date | Measurement | Setup | Result |
|---|---|---|---|
| _pending_ | PC → archive.org download speed (HANDOFF §8.1) | `curl -r 0-104857599` against a storage server | |
