# Architecture

This document describes how xiso-cartographer is built and what each part is responsible for. It must stay in sync with the code: an MR that changes a layer, module, interface or data flow updates this file in the same MR. Decisions and their reasons live in the [ADRs](adr/); formats are specified in [formats/](formats/). Goals and roadmap are in [HANDOFF.md](HANDOFF.md).

## 1. Overview

```mermaid
flowchart LR
  subgraph Offline["Catalog pipeline (PC, occasional)"]
    S[Scraper + normaliser] --> P[catalog.bin + thumbnails + CREDITS]
  end
  A[(archive.org<br/>HTTPS + Range)]
  L[(LAN HTTP server<br/>with Range)]
  subgraph Xbox["xiso-cartographer.xbe (nxdk)"]
    UI[UI] --> CAT[catalog]
    UI --> MAP[map + sources]
    UI --> JOB[install job]
    JOB --> NET[http + tls]
    JOB --> XD[xdvdfs reader]
    XD --> EX[extractor]
    EX --> FS[fatx sink]
  end
  P -->|bundled in release| CAT
  A --> NET
  L --> NET
```

The user picks a game from the **catalog**, the **map** says where the user's image of it is, and an **install job** stream-extracts that image over HTTP(S) into `<drive>:\Games\<title>\` ([ADR 0004](adr/0004-streaming-install.md)).

## 2. Layers

| Layer | Path | Contains | Depends on |
|---|---|---|---|
| Core (`libxcat`) | `core/` | All parsing and logic. Portable C11, no platform headers. | C standard library only, plus BearSSL from M5 |
| Platform | `platform/` | Thin adapters that implement the core's interfaces (§4): `nxdk/` for the Xbox, `host/` for POSIX/Windows (tests, CLI). | Core interfaces; nxdk or the host OS |
| App | `app/` | The XBE: UI, screens, job orchestration. No parsing. | Core, platform/nxdk |
| Tools | `tools/` | `xcat-cli` (runs the core on a PC), the fixture generator ([`tools/fixtures/`](../tools/fixtures/README.md), Python), catalog pipeline. | Core, platform/host (the fixture generator is standalone Python) |

**Rules** (enforced in review):

- `core/` never includes nxdk or OS headers, and never touches global mutable state. Every function takes a context struct.
- Parsers live in `core/` only.
- All external input (images, HTTP responses, JSON, map files) is untrusted and parsed with checked read helpers (HANDOFF §7.3).

## 3. Core modules

Status: ✅ exists · 🔜 next milestone · ⏳ later.

| Module | Responsibility | Milestone | Status |
|---|---|---|---|
| `version` | Library version (`xc_version_string`, `xc_version_number`) | M0 | ✅ |
| `error` | `xc_err` result codes and user-readable messages (`xcat/error.h`) | M1 | ✅ |
| `source` | `xc_source` random-access byte source + in-memory source (`xcat/source.h`) | M1 | ✅ |
| `xdvdfs` | Find the game partition (xiso, XGD1/2/3) and walk the directory trees; produce an offset-sorted file list ([xdvdfs.md](formats/xdvdfs.md), `xcat/xdvdfs.h`) | M1 | ✅ |
| `extract` | Pull byte ranges from a `ByteSource` and write them to a `Sink`; coalesce ranges; journal and resume; verify | M1 (local), M4 (network) | 🔜 |
| `fatxname` | Sanitise names to FATX rules (≤ 42 chars, allowed character set); prevent collisions | M2 | ⏳ |
| `http` | HTTP/1.1 client: `Range`, cross-host redirects (max 5), chunked transfer, `Content-Range` checks, timeouts, backoff | M3 | ⏳ |
| `tls` | BearSSL wrapper: TLS 1.2, SNI, CA bundle, time from `xc_clock` ([ADR 0005](adr/0005-bearssl-for-tls.md)) | M5 | ⏳ |
| `ntp` | Minimal SNTP client, so certificate checks have a correct clock | M5 | ⏳ |
| `hash` | MD5/SHA-1 with exportable state (from BearSSL) | M5 | ⏳ |
| `map` | Parse map files: entries, `@include`, `@archiveorg` ([map.md](formats/map.md)) | M8 (parser can land earlier) | ⏳ |
| `sources/archiveorg` | Metadata API client (streaming JSON); builds map entries, supplies sizes and hashes | M6 | ⏳ |
| `catalog` | Load and search `catalog.bin` (versioned binary format) | M7 | ⏳ |

Public headers: `core/include/xcat/<module>.h`. Symbols: `xc_` prefix. Errors: functions return `xc_err` and use out-params for results; every error maps to a user-readable message in the app.

## 4. Platform interfaces

The core reaches the outside world only through small structs of function pointers. The platform layer fills them in, and tests pass fakes. Planned interfaces:

| Interface | Provides | Implemented by |
|---|---|---|
| `xc_io` | open/read/write/seek/close files, create directories, free space | nxdk (FATX), host (stdio/POSIX) |
| `xc_net` | TCP connect/send/recv/close with timeouts, DNS | nxdk (lwIP), host (sockets) |
| `xc_clock` | wall-clock time (for TLS) and a monotonic clock (for timeouts) | nxdk (kernel time + NTP), host |
| `xc_log` | levelled logging | nxdk (`E:\Apps\xcat\xcat.log`, rotated), host (stderr) |

The streaming abstractions used by `extract`:

- **`ByteSource`** = [`xc_source`](../core/include/xcat/source.h): "read exactly `[offset, offset+len)`", with the total size known up front. Backed by memory (tests, fuzzing), a local file (host tests now; the Xbox HDD in M2) or HTTP Range (M4, M5).
- **`Sink`:** "create this file, append these bytes, close it". Backed by the host filesystem (tests, CLI) or FATX (Xbox).

The exact signatures are defined in the MR that adds each interface. This section is then updated to link the header.

## 5. Install flow

1. Resolve the map entry for the chosen catalog item to a URL ([map.md](formats/map.md)).
2. Probe the URL: follow redirects, and require `206` with a matching `Content-Range`.
3. Fetch the volume descriptor and directory tables. Build the file list and total size.
4. Check free space on the target drive.
5. Create `<drive>:\Games\<FATX-safe title>\` and open the journal.
6. For each file in offset order (neighbours coalesced): request its range and write it as it streams in. Journal it when done.
7. Verify sizes, plus the whole-image hash for trimmed xisos when the source provides one ([ADR 0004](adr/0004-streaming-install.md)).
8. Write the `.xcat-installed` marker and dashboard artwork, then remove the journal.

On resume, skip journaled files and rewrite the partial one from its start.

## 6. Memory budget

64 MB is a hard ceiling (HANDOFF §7.2). Allocation happens once at job start; nothing allocates per chunk.

| Item | Size | Notes |
|---|---|---|
| I/O buffer | 256 KB–1 MB | One per stage. Final size set by the M3 benchmark. |
| TLS buffer | ~33 KB per connection | BearSSL bidirectional buffer; at most 2 connections |
| Directory list | Proportional to file count | Bounded by an entry-count limit in `xdvdfs` |
| Catalog | < 2 MB text + indices | Thumbnails loaded on demand from the HDD |

Peak memory during an install is logged on the Xbox and recorded in [testing.md](testing.md).

## 7. Builds

| Build | Tooling | Output | Docs |
|---|---|---|---|
| Host (core + tests) | CMake presets, Ninja; gcc/clang/MSVC/clang-cl | `build/<preset>/` | [CONTRIBUTING.md](../CONTRIBUTING.md#host-build-core-library-and-tests) |
| XBE | nxdk in Docker (`make docker-xbe`) | `app/bin/default.xbe`, `app/xiso-cartographer.iso` | [build-xbox.md](build-xbox.md) |

`app/Makefile` compiles the `core/` sources it needs straight into the XBE, so there's no separate library build for the Xbox.
