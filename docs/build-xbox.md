# Building and running the XBE

The Xbox app is built with [nxdk](https://github.com/XboxDev/nxdk), pinned as the submodule `third_party/nxdk`. The supported way to build it is inside a Docker image made from that submodule, so every machine and CI use the same toolchain.

## Prerequisites

- Docker (on Windows: Docker Desktop with WSL 2).
- The nxdk submodule and its own submodules:

  ```sh
  git submodule update --init --recursive
  ```

### Windows: line endings

If git converts line endings on checkout (`core.autocrlf=true`, the Git for Windows default), nxdk's shell scripts get CRLF endings and the Linux build fails with `env: can't execute 'sh'`. Our own files are protected by `.gitattributes`, but nxdk and its submodules aren't. Run this once after cloning, and again after any submodule update that adds new submodules:

```sh
git submodule foreach --recursive "git config core.autocrlf false && git config core.eol lf && git rm -q --cached -r . && git reset -q --hard"
```

This rewrites the submodules' working files only. It's safe as long as you haven't edited anything inside `third_party/`.

## Build

With `make` (Linux, macOS, WSL):

```sh
make docker-xbe
```

Without `make` (e.g. Git Bash or PowerShell on Windows):

```sh
docker build -f docker/nxdk.Dockerfile -t xcat-nxdk --build-context xcat-lwip=platform/nxdk/lwip third_party/nxdk
docker run --rm -v "$PWD:/usr/src/app" xcat-nxdk make -C app
```

On Git Bash, prefix the `docker run` command with `MSYS_NO_PATHCONV=1`, so the container path isn't rewritten.

The first image build compiles nxdk's tools and libraries. Docker caches it after that, until the nxdk submodule or our lwIP options change.

**Network (lwIP) options:** nxdk's network library is built into the image with our [`platform/nxdk/lwip/lwipopts.h`](../platform/nxdk/lwip/lwipopts.h) ahead of nxdk's own: a larger TCP receive window with window scaling, more receive buffers in the network driver, and SACK. Without them one connection to archive.org ran at 0.3 MB/s (testing.md, M5). `app/lwip.mk` puts the same folder first for our own sources, so both sides agree. The file reaches the image build as the named context `xcat-lwip` (the `--build-context` above); after changing it, rebuild the image (`make docker-xbe` does).

**Output:**

| File | Use |
|---|---|
| `app/bin/default.xbe` | The program. Copy it to the console. |
| `app/bin/samples/small.iso` | Bundled synthetic test image (the `small` fixture: dummy data, never a game). The app installs it when `E:\xcat\isos\` is empty. |
| `app/xiso-cartographer.iso` | Disc image containing `app/bin/` (the XBE and `samples/`). Boots directly in xemu. |
| `app/fatx-probe/bin/default.xbe`, `app/fatx-probe/xcat-fatx-probe.iso` | The FATX probe: a test tool that measures what file names and paths the console's FATX driver accepts. See [testing.md](testing.md#fatx-probe-m2). |
| `app/net-test/bin/default.xbe`, `app/net-test/xcat-net-test.iso` | The network test: downloads a file over HTTP or HTTPS and reports MB/s, retries and the CRC-32; for HTTPS also the CPU's cipher speed and the negotiated TLS suite. See [testing.md](testing.md#m3-network-download-test) and [M5](testing.md#m5-tls-speed-on-the-console). |

XBEs with HTTPS include [`app/tls.mk`](../app/tls.mk): BearSSL, `core/src/tls.c`, `core/src/sntp.c`, `core/src/hash.c`, the CA bundle (embedded at build time, so the build needs `python3`, which the image has) and `platform/nxdk/src/nx_tls.c`. nxdk compiles without optimisation by default; `tls.mk` builds BearSSL and `tls.c` with `-O2`, since unoptimised crypto would understate the TLS speed several times over. BearSSL's object files land in its submodule folder, which git is told to ignore (`ignore = untracked` in `.gitmodules`).

Every CI run also uploads all of these files as the `xiso-cartographer-xbe-<sha>` artifact (`xbe` job).

## Run in xemu

1. Set up xemu with your own BIOS, MCPX boot ROM and HDD image. These are never part of this repo; see the [xemu docs](https://xemu.app/docs/required-files/).
2. **Machine → Load Disc…** and choose `app/xiso-cartographer.iso`. Then reset the machine (**Machine → Reset**) if it doesn't boot on its own.

## What the app does (M5)

Until the real UI arrives (M8), the app installs everything it's given, one after another:

1. **Every entry of the map `E:\xcat\map.txt`** ([map.md](formats/map.md)), downloaded over HTTP and installed while it streams in. Nothing is stored except the game's files ([ADR 0004](adr/0004-streaming-install.md)), so images of 4 GiB and more, such as full-disc dumps, install too. Entries that need a login use `E:\Apps\xcat\credentials` ([credentials.md](formats/credentials.md)). Entries can be `http://` or `https://` (see [HTTPS](#https) below).
2. **Every image in `E:\xcat\isos\`**, as in M2. These must be under 4 GiB, since FATX can't store a larger file.
3. If there's neither, the bundled test image from `D:\samples\` (`D:` is the folder or disc the XBE was started from), copied to `E:\xcat\isos\` first.

Each install, network or local, follows the same steps ([ARCHITECTURE.md §5](ARCHITECTURE.md#5-install-flow)):
- It checks that FATX can store every file under `E:\Games\<title>` ([ADR 0008](adr/0008-refuse-games-fatx-cant-store.md)) and that there's enough free space.
- It writes the files with progress and a **journal**, so a pulled cable is retried and a **power cut resumes** on the next start instead of starting over ([journal.md](formats/journal.md)).
- For a trimmed xiso it reads the image front to back in full and computes its **MD5 and SHA-1**, which it logs (from M6 they're compared with archive.org's; [journal.md §7](formats/journal.md#7-verification)). Full-disc layouts are checked by size and CRC-32 only.
- It **reads every installed file back and compares its CRC-32** with what was received, then writes `E:\Games\<title>\.xcat-installed`, with the hashes when there are some.

A title that's already installed gets a numbered folder (`small (2)`). An unfinished install of the same image is resumed in its own folder; one of a different image is left alone. The run ends with "Done: N of M image(s) installed and verified." Everything on screen is also appended to `E:\xcat\install.log`.

### HTTPS

The first `https://` URL (a map entry, an `@include`, or a redirect from `http://`) sets up TLS once ([ADR 0012](adr/0012-tls-trust-time-and-entropy.md)), and the log shows each step:
- **Randomness** from timer jitter and `E:\Apps\xcat\random-seed`, which it renews.
- **The time** for certificate checks: from NTP, `pool.ntp.org` by default ([sntp.md](formats/sntp.md)). To change it, put a host name or IPv4 address on the first line of `E:\Apps\xcat\ntp.txt`, or `off` to skip NTP (until the settings screen, M8). If NTP fails, the console's clock is used, but only if it isn't earlier than the XBE's build. Otherwise HTTPS stops with "Couldn't get the correct time (NTP server unreachable and the console's clock is wrong)"; `http://` entries still install.
- **Trust:** the built-in Mozilla roots, or only the certificates in `E:\Apps\xcat\cacert.pem` if that file exists (for a LAN server with its own certificate).

After each HTTPS install the log shows the TLS version and suite the server chose.

**Hold LT + RT + Back + Black** (the shortcut rocky5's tools use) at any time to return to the dashboard, including mid-install or while it waits out a network outage. An install stopped this way keeps everything written so far and resumes on the next start. Confirmed on hardware on 2026-10-15 ([testing.md](testing.md#results-log)).

## Run on hardware

1. Start the console's FTP server from your dashboard. On XBMC4Gamers and UnleashX it's normally enabled; the default login is `xbox` / `xbox`.
2. Connect with an FTP client (e.g. FileZilla) to the console's IP on port 21.
3. Create `E:\Apps\xiso-cartographer\` and upload the contents of `app/bin/` into it: `default.xbe` and the `samples` folder. Use **binary** transfer mode.
4. Optionally, upload your own images (xiso, under 4 GiB) to `E:\xcat\isos\`.
5. Launch it from the dashboard's Applications list, or browse to the folder in the file manager.

Record each run in [testing.md](testing.md).
