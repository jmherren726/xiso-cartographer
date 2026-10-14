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
docker build -f docker/nxdk.Dockerfile -t xcat-nxdk third_party/nxdk
docker run --rm -v "$PWD:/usr/src/app" xcat-nxdk make -C app
```

On Git Bash, prefix the `docker run` command with `MSYS_NO_PATHCONV=1`, so the container path isn't rewritten.

The first image build compiles nxdk's tools and libraries. Docker caches it after that, until the nxdk submodule changes.

**Output:**

| File | Use |
|---|---|
| `app/bin/default.xbe` | The program. Copy it to the console. |
| `app/bin/samples/small.iso` | Bundled synthetic test image (the `small` fixture: dummy data, never a game). The app installs it when `E:\xcat\isos\` is empty. |
| `app/xiso-cartographer.iso` | Disc image containing `app/bin/` (the XBE and `samples/`). Boots directly in xemu. |
| `app/fatx-probe/bin/default.xbe`, `app/fatx-probe/xcat-fatx-probe.iso` | The FATX probe: a test tool that measures what file names and paths the console's FATX driver accepts. See [testing.md](testing.md#fatx-probe-m2). |

Every CI run also uploads both files as the `xiso-cartographer-xbe-<sha>` artifact (`xbe` job).

## Run in xemu

1. Set up xemu with your own BIOS, MCPX boot ROM and HDD image. These are never part of this repo; see the [xemu docs](https://xemu.app/docs/required-files/).
2. **Machine → Load Disc…** and choose `app/xiso-cartographer.iso`. Then reset the machine (**Machine → Reset**) if it doesn't boot on its own.

## What the app does (M2)

Until the real UI arrives (M8), the app installs every image it finds in `E:\xcat\isos\`:

1. It creates `E:\xcat\isos\` and `E:\Games\` if they're missing. If `E:\xcat\isos\` has no `.iso` files, it first copies the bundled test image from `D:\samples\`. `D:` is the folder (or disc) the XBE was started from.
2. For each image, it reads the image, checks that FATX can store every file under `E:\Games\<title>` ([ADR 0008](adr/0008-refuse-games-fatx-cant-store.md)) and that there's enough free space, then extracts it with progress.
3. It **reads every installed file back and compares it byte for byte with the image**, then writes `E:\Games\<title>\.xcat-installed`.
4. It ends with a summary: "Done: N of M image(s) installed and verified." A title that's already installed gets a numbered folder (`small (2)`).

Images must be uncompressed xiso or full-disc `.iso` files **under 4 GiB**: FATX can't store a larger file, so full-disc dumps (about 7.8 GB) can't sit on the hard disk. Those will install over the network instead (M4/M5).

## Run on hardware

1. Start the console's FTP server from your dashboard. On XBMC4Gamers and UnleashX it's normally enabled; the default login is `xbox` / `xbox`.
2. Connect with an FTP client (e.g. FileZilla) to the console's IP on port 21.
3. Create `E:\Apps\xiso-cartographer\` and upload the contents of `app/bin/` into it: `default.xbe` and the `samples` folder. Use **binary** transfer mode.
4. Optionally, upload your own images (xiso, under 4 GiB) to `E:\xcat\isos\`.
5. Launch it from the dashboard's Applications list, or browse to the folder in the file manager.

Record each run in [testing.md](testing.md).
