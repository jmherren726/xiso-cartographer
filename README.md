# xiso-cartographer

Homebrew game catalog and installer for the modded original Xbox. Browse the full library, then download and install your own game backups directly on the console, with streaming xiso extraction, HTTPS and resumable installs. Built with [nxdk](https://github.com/XboxDev/nxdk).

## Status

**Pre-alpha. Nothing runs yet.** The repository is being scaffolded. See the [roadmap](docs/HANDOFF.md#41-roadmap) for the milestones and [CHANGELOG.md](CHANGELOG.md) for what has landed.

## What it will do

- Run on a soft- or hard-modded Xbox (64 or 128 MB), launched from common dashboards such as XBMC4Gamers or UnleashX.
- Show a catalog of original Xbox titles (title, region, genre, description, cover art, size) built from openly licensed metadata.
- Read a **map** that you write, which says where each of your disc images lives: an archive.org item you have access to, or a plain HTTP server on your LAN.
- Download an uncompressed `.xiso.iso` image and **stream-extract** it straight into `<drive>:\Games\`, with no temporary copy and no PC. Installs are resumable and verified by hash when the source provides one.

## Legal stance

xiso-cartographer is a tool for installing **backups you are entitled to use**.

- It ships with **no game images, download links, ROMs, BIOS/MCPX dumps or Microsoft XDK material**, and none will be accepted into this repository, its issues or its CI.
- Every source is supplied by the user through their own map file.
- It will never include code that gets past captchas, Cloudflare or any other bot or access protection, or that violates a source's terms.
- It is built only with the open-source nxdk toolchain.

## Building

There is nothing to build yet. The host build (CMake + CTest) arrives in MR-2 and the nxdk XBE build in MR-4. This section will then hold the quick start.

## Documentation

| Doc | Contents |
|---|---|
| [docs/HANDOFF.md](docs/HANDOFF.md) | Goals, design decisions, architecture, roadmap and standards |
| [docs/adr/](docs/adr/) | Architecture decision records |
| [CONTRIBUTING.md](CONTRIBUTING.md) | Setup, MR rules and review checklist |
| [CLAUDE.md](CLAUDE.md) | Guidance for AI coding agents |

## Licence

[MIT](LICENSE). See [ADR 0002](docs/adr/0002-license.md) for the reasoning and dependency licences.
