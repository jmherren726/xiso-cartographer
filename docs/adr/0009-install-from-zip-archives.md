# 0009. Install from zip archives by streaming the zip itself

- **Status:** Accepted (2026-10-14, by the owner). Zip support was optional milestone M9b; it becomes **M5b**, built right after M5 (HTTPS).
- **Date:** 2026-10-14

## Context

The owner's sources on archive.org are zip files, each holding one full-disc `.iso`. Two ways to get the ISO out were tested with curl on 2026-10-14, logged in with the owner's own session cookies scoped to `.archive.org` ([ADR 0007](0007-scoped-user-credentials.md)). Item names are left out, per [ADR 0006](0006-no-links-no-bypass.md).

**1. The inner-file URL** (`/download/<item>/<file>.zip/<file>.iso`):

- It answers `302` to `view_archive.php` on a storage host, which **extracts the member on the fly**.
- Without the login it returns `403`; with it, `200` with `X-Access-Controlled: 1`.
- The response is `Transfer-Encoding: chunked` with **no `Content-Length`**, and it **ignores `Range`**: a request for bytes from 1 GiB on gets `200` and the stream from the start.
- In the browser, the download **stops at about 1 GB, every time**.

So it can't be resumed, verified or used for a streaming install, which requires `Range` ([ADR 0004](0004-streaming-install.md)).

**2. The zip itself** (`/download/<item>/<file>.zip`):

- It answers `302` to a storage host (`dnNNNNNN.ca.archive.org`), then **`206 Partial Content`** with `Content-Range: bytes 0-1023/5739306907`. The login cookie travels across the redirect because it's scoped to `.archive.org`.
- Its local file header shows **method 8 (Deflate)**, flags `0x2` (maximum compression; no trailing data descriptor), and both sizes `0xFFFFFFFF`, so the real sizes are in a **Zip64** extra field. A ~7.8 GB full-disc ISO is stored in a 5.74 GB zip.

## Decision

1. **The installer reads zip archives through HTTP `Range`**, like images:
   - read the end of the zip to find the central directory (including the Zip64 end records);
   - pick the member (below);
   - read its local header;
   - **stream-decompress the member's data in one sequential pass**, feeding the decompressed bytes to the existing XDVDFS reader and extractor.
2. **Supported members:** method 0 (stored) and method 8 (Deflate), with Zip64. Anything else is refused with a clear message: Deflate64 (9), LZMA (14), encrypted members, or split archives.
3. **Member choice:** use the zip's only `.iso` / `.xiso` member. If there are several, the install is refused and they're listed. A map syntax for choosing one comes later if needed.
4. **Stored members** are just a byte range in the zip, so they install exactly like a bare image: Range reads, gap skipping.
5. **Deflate members** can't be read at random positions, so the whole member is decompressed in order:
   - For a full-disc image, that means downloading the whole zip. Decompressed bytes outside the game's files (the video partition, gaps) are discarded.
   - The upside is that **every installed byte passes through the member's CRC-32**, so full-disc installs get complete verification against the zip, stronger than ADR 0004's size-only check for Redump layouts.
6. **Checkpoints, resume and late tables:** while decompressing, the installer saves a checkpoint every 64 MiB of decompressed output: miniz's `tinfl` state plus its 32 KiB window (about 43 KiB), the compressed offset, and the CRC-32 of that interval. Checkpoints serve two purposes:
   - **Resume:** an interrupted install restarts the download from the last checkpoint with `Range`.
   - **Files that come before their directory table** (see "Directory-table order" below): pass 1 streams the whole member and writes every file whose tables have already been read. Once all tables are known, pass 2 restarts from the checkpoint before the first late file, re-downloading only up to the last table those files wait for, and writes the late files. Re-read bytes are checked against pass 1's per-interval CRC-32s.
7. **Library:** miniz (MIT; already planned in [ADR 0002](0002-license.md)), behind a small `core/zip` module with its own fuzz target, like every parser (HANDOFF §7.3).
8. **Maps:** `…zip/<member>` URLs (archive.org's on-the-fly extraction) are rejected when a map is loaded, with a message that points to the zip URL. The map spec gains this rule in the MR that adds zip support.

## Directory-table order (measured)

Installing in one sequential pass needs every directory table to come before the files it describes. Otherwise a file's bytes pass by before the installer knows which file they belong to, and a Deflate stream can't seek back. Trimmed xisos from extract-xiso (and our fixtures) put all tables first.

**A retail full-disc dump doesn't** (one title, measured 2026-10-14 with `xcat-cli layout`, [xdvdfs.md §8](../formats/xdvdfs.md#8-stream-order)):

| | Value |
|---|---|
| Layout | XGD1, partition at 387.0 MiB, image 7,825,162,240 bytes |
| Directory tables | 40, between 1,167.9 and 4,040.5 MiB; the root table too |
| File data | 517 files, 4,054.2 MiB, between 410.8 and 5,118.1 MiB |
| Late files | **8 files, 616.4 MiB**, starting from 410.8 MiB; they wait for tables ending at 1,244.3 MiB |

So the mastering puts some large files ahead of the directory tables. Only 8 of 517 files are affected, but a single pass can't install this image.

**Decision:** checkpoints and a targeted second pass (point 6). For this image, pass 2 re-reads 410.8 → 1,244.3 MiB of the image: about 834 MiB decompressed, roughly 0.6 GB of the 5.74 GB zip at its average 73% ratio, or ~10% extra download. It needs about 5 MB of checkpoints on disk.

**Alternatives considered:**

- **Spooling every unidentified byte to disk** until all tables are known: no extra download, but up to ~1.4 GB of temporary disk for this image (the late files plus every gap, since gaps can't be told from late files until the last table is read). Too much for a stock 8 GB drive.
- **Two full passes:** +4–5.7 GB of download.

Only Deflate members are affected. Bare images and stored members are read with `Range`, tables first, whatever the order. The installer shows the extra cost (from the same report) before it starts.

## Consequences

- The owner's sources become installable directly on the Xbox, with no PC step and no 1 GB stop.
- A full-disc zip install downloads the whole zip (5.74 GB for the tested item), versus about 7.8 GB uncompressed. At the hoped-for 1–3 MB/s that's roughly 30–95 minutes. Deflate is much faster than the network on the Xbox CPU, so it shouldn't be the bottleneck. M5 measures the network side.
- Memory: about 43 KiB of decompressor state, plus the existing I/O buffer.
- Some zips will be refused (Deflate64 from Windows' built-in zipper, LZMA from 7-Zip's zip mode). Each refusal names the method.
- Adds `core/zip`, miniz, a fuzz target, Zip64 test fixtures (the fixture generator gains zip output), and changes to the map spec and `extract` (a sequential-source mode).
