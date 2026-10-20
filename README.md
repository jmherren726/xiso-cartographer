# xiso-cartographer

[![CI](https://github.com/jmherren726/xiso-cartographer/actions/workflows/ci.yml/badge.svg)](https://github.com/jmherren726/xiso-cartographer/actions/workflows/ci.yml)

Homebrew game catalog and installer for the modded original Xbox. Browse the full library, then download and install your own game backups directly on the console, with streaming xiso extraction, HTTPS and resumable installs. Built with [nxdk](https://github.com/XboxDev/nxdk).

## Status

**Pre-alpha, installing real games on hardware.** The install engine is done and has been tested on a real console. The catalog UI isn't built yet: for now the app shows a plain text list. Milestones M0–M6 are met on hardware. What works on the Xbox today:

- **Installs from your map, over HTTP or HTTPS:** TLS 1.2 with certificate checks against built-in roots, the clock set from NTP, and archive.org logins from a [credentials file](docs/formats/credentials.md). Files are stream-extracted straight into `E:\Games\<title>`, with no temporary copy, so full-disc images of 7+ GB install too.
- **Images and zips:** trimmed xisos, full-disc `.iso` dumps, and `.zip` files holding one image, stored or Deflate (including Zip64). Deflate zips are decompressed as they download.
- **Resumable and verified:** a pulled cable is retried, and a power cut resumes where it stopped, even in the middle of a Deflate zip. Every file is read back and checked against its CRC-32, and trimmed xisos against their MD5 and SHA-1.
- **archive.org items:** `@archiveorg <item>` in the map lists every game the item holds, with the size and hashes it publishes.
- **You choose:** the map is a catalog. The app lists it, and installs only the game you pick with A.

Not there yet: the catalog with metadata and cover art (M7), the real UI and logging in on the console (M8). See the [roadmap](docs/HANDOFF.md#41-roadmap) for the milestones, [docs/testing.md](docs/testing.md#results-log) for what was tested on hardware, and [CHANGELOG.md](CHANGELOG.md) for what has landed. To try it, see [What the app does](docs/build-xbox.md#what-the-app-does-m6).

## What it will do

- Run on a soft- or hard-modded Xbox (64 or 128 MB), launched from common dashboards such as XBMC4Gamers or UnleashX.
- Show a catalog of original Xbox titles (title, region, genre, description, cover art, size) built from openly licensed metadata.
- Read a **map** that you write, which says where each of your disc images lives: an archive.org item you have access to, or a plain HTTP server on your LAN.
- Download a disc image (a trimmed xiso, a full-disc `.iso` dump, or a `.zip` holding one) and **stream-extract** it straight into `<drive>:\Games\`, with no temporary copy and no PC. Installs are resumable and verified by hash when the source provides one.

## Legal stance

xiso-cartographer is a tool for installing **backups you are entitled to use**.

- It ships with **no game images, download links, ROMs, BIOS/MCPX dumps or Microsoft XDK material**, and none will be accepted into this repository, its issues or its CI.
- Every source is supplied by the user through their own map file.
- It will never include code that gets past captchas, Cloudflare or any other bot or access protection, or that violates a source's terms.
- It is built only with the open-source nxdk toolchain.

## Building

There are two builds: the portable core library with its unit tests (CMake, on a PC), and the Xbox app (nxdk, in Docker). CI builds both on every pull request, and its `xiso-cartographer-xbe-<commit>` artifact holds the app.

```sh
git clone --recursive https://github.com/jmherren726/xiso-cartographer.git
cd xiso-cartographer
cmake --preset host-debug
cmake --build --preset host-debug
ctest --preset host-debug
```

You need CMake 3.25+, Ninja, Python 3.10+ and a C11 compiler: gcc or clang on Linux/macOS, or MSVC/clang-cl on Windows. See [CONTRIBUTING.md](CONTRIBUTING.md#setup) for details.

The host build also produces **`xcat-cli`**, which runs the core on a PC. It works on both trimmed xisos and full-disc `.iso` dumps:

```sh
build/host-debug/tools/xcat-cli/xcat-cli list <image>               # layout + every file, in disc order
build/host-debug/tools/xcat-cli/xcat-cli extract <image> <out-dir>  # same output as extract-xiso -x
build/host-debug/tools/xcat-cli/xcat-cli check <image> [title]      # can the Xbox HDD store it?
build/host-debug/tools/xcat-cli/xcat-cli layout <image>             # can it be installed in one pass?
build/host-debug/tools/xcat-cli/xcat-cli get <url> <file>           # download over HTTP(S) with Range and resume
build/host-debug/tools/xcat-cli/xcat-cli map <map-file>             # check a map file and list its entries
build/host-debug/tools/xcat-cli/xcat-cli install <url|image> <games-dir> [title]   # install like the console: resumable, verified
```

`get`, `map` and `install` speak `http://` and `https://` (TLS 1.2, certificates always checked against the built-in Mozilla roots, or the PEM file given with `--ca-file`), and report retries and MB/s on the way. `--credentials <file>` adds logins from a [credentials file](docs/formats/credentials.md), sent only to the hosts each one is scoped to. [tools/testserver](tools/testserver/README.md) has a local Range server, with optional faults, to try it against.

**Xbox app** (needs Docker):

```sh
git submodule update --init --recursive
make docker-xbe     # → app/bin/default.xbe, app/xiso-cartographer.iso
```

See [docs/build-xbox.md](docs/build-xbox.md) for building without `make`, the Windows line-ending fix, and running the XBE in xemu or on a console.

## Documentation

| Doc | Contents |
|---|---|
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Layers, modules, interfaces, install flow, memory budget |
| [docs/formats/map.md](docs/formats/map.md) | The map file format (where your images are) |
| [docs/formats/archiveorg.md](docs/formats/archiveorg.md) | `@archiveorg`: turning an archive.org item's listing into map entries |
| [docs/formats/credentials.md](docs/formats/credentials.md) | Logins for sources that need them: host-scoped tokens, kept out of maps |
| [docs/formats/zip.md](docs/formats/zip.md), [stream-install.md](docs/formats/stream-install.md) | Installing from zips, and from Deflate streams that can only be read front to back |
| [docs/formats/http.md](docs/formats/http.md) | The HTTP client: Range requests, redirects, retries and resume |
| [docs/formats/fatx-names.md](docs/formats/fatx-names.md) | What the Xbox hard disk can store, measured on the console, and when an install is refused |
| [docs/formats/journal.md](docs/formats/journal.md) | The install journal that lets an interrupted install resume |
| [docs/HANDOFF.md](docs/HANDOFF.md) | Goals, design decisions, roadmap and standards |
| [docs/build-xbox.md](docs/build-xbox.md) | Building the XBE and running it in xemu or on hardware |
| [docs/testing.md](docs/testing.md) | Test ladder, hardware bench and results log |
| [docs/adr/](docs/adr/) | Architecture decision records |
| [CONTRIBUTING.md](CONTRIBUTING.md) | Setup, MR rules and review checklist |
| [CLAUDE.md](CLAUDE.md) | Guidance for AI coding agents |

## Licence

[MIT](LICENSE). See [ADR 0002](docs/adr/0002-license.md) for the reasoning and dependency licences.
