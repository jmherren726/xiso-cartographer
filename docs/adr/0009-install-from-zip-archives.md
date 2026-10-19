# 0009. Install from zip archives by streaming the zip itself

- **Status:** Accepted (2026-10-14, by the owner). Zip support was optional milestone M9b; it becomes **M5b**, built right after M5 (HTTPS). **Amended 2026-10-19** (owner decision): late files go through an adaptive spool, then a second pass over what it dropped. [Amendment 1](#amendment-1-2026-10-19-adaptive-spool) replaces point 6's second-pass rule and the "Directory-table order" decision.
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
   - **Files that come before their directory table** (see "Directory-table order" below; *replaced by [Amendment 1](#amendment-1-2026-10-19-adaptive-spool)*): pass 1 streams the whole member and writes every file whose tables have already been read. Once all tables are known, pass 2 restarts from the checkpoint before the first late file, re-downloading only up to the last table those files wait for, and writes the late files. Re-read bytes are checked against pass 1's per-interval CRC-32s.
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

**Decision (original; replaced by [Amendment 1](#amendment-1-2026-10-19-adaptive-spool), which measured more games):** checkpoints and a targeted second pass (point 6). For this image, pass 2 re-reads 410.8 → 1,244.3 MiB of the image: about 834 MiB decompressed, roughly 0.6 GB of the 5.74 GB zip at its average 73% ratio, or ~10% extra download. It needs about 5 MB of checkpoints on disk.

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

## Amendment 1 (2026-10-19): adaptive spool

### Why

The original decision was sized on one game, where 8 of 517 files came before their tables and a second pass cost ~10% extra download. Measurements since then (`xcat-cli layout`, retail XGD1 full-disc dumps from the owner, titles left out per [ADR 0006](0006-no-links-no-bypass.md)) show that was the good case:

| Game | Late files / all | Late file data | Last table ends at | Unidentified before that |
|---|---|---|---|---|
| A (2026-10-14) | 8 / 517 | 616 MiB | 4,040.5 MiB | not measured (before §8.1) |
| B (2026-10-14) | 20 / 21 | ~2,968 MiB | ~3,738 MiB | not measured |
| C | 12 / 14 | 868 MiB | 3,732.9 MiB | 3,346 MiB |
| D | 2,113 / 2,117 | 2,798 MiB | 3,735.6 MiB | 3,348 MiB |
| E | 19 / 27 | 1,757 MiB | 2,861.4 MiB | 2,474 MiB |
| F | 59 / 59 | 218 MiB | 3,737.8 MiB | 3,351 MiB |

Every game has late files. In most of them, most of the game is late, so a second pass alone means downloading most of the game twice (D re-reads 3,288 MiB of the image).

The measurements show two things ([xdvdfs.md §8.1](../formats/xdvdfs.md#81-spool-plan) defines the terms):

1. The bytes that can't be identified yet start with **padding**: hundreds of MiB to ~3 GiB between the partition start and the first file data. The late files sit **just before the tables** that list them.
2. So a spool that keeps the **latest** unidentified bytes, dropping its oldest when full, keeps the late files. A spool that keeps the **first** bytes fills up with padding and saves almost nothing: C, E and F re-read the same with 2 GiB of spool as with none.

Pass 2 re-reads, in MiB of the image, by spool size (keep-latest model; pass 2 starts at a 64 MiB checkpoint):

| Spool | C | D | E | F |
|---|---|---|---|---|
| 0 | 917 | 3,288 | 1,944 | 282 |
| 256 MiB | 661 | 3,032 | 1,709 | 0 |
| 512 MiB | 405 | 2,776 | 1,453 | 0 |
| 1 GiB | 0 | 2,264 | 941 | 0 |
| 2 GiB | 0 | 1,240 | 0 | 0 |
| everything (no pass 2) | 3,346 | 3,348 | 2,474 | 3,351 |

No single size suits every game and every drive, so the spool uses whatever disk the install doesn't need.

### Decision

This applies to **Deflate members** only. Bare images and stored members are read with `Range`, tables first, as before.

1. **Pass 1** decompresses the whole member in order, as in point 5:
   - bytes of a table, or of a file whose directory is already known, are handled as now: tables are parsed, file bytes are written;
   - every other byte between the partition start and the end of the last table is **unidentified**: a gap or part of a late file, which can't be told apart yet. It goes to the **spool**;
   - bytes before the partition (the video partition), and bytes after the last table that belong to no file, are discarded.
2. **The spool is a set of chunk files**, one per 64 MiB checkpoint interval of the image (`XC_INSTALL_CHECKPOINT`), in the install folder next to the journal. A chunk holds its interval's unidentified bytes and is complete once the stream has passed that interval.
3. **Size: free disk minus a reserve.** The spool has no fixed size. Before each write (game file or spool), the installer checks that the free space on the target drive stays above a **reserve** (256 MiB to start, tuned after the hardware tests). If it wouldn't, it **deletes the oldest spool chunk** and checks again. Game files always win over the spool. Since dropped chunks are always the oldest, together they cover one range from the partition start, `[partition, stop)`.
4. **Once every table has been read**, the whole layout is known:
   - the free-space check from the bare-image install runs, and with it the FATX checks (ADR 0008). If the game doesn't fit, the install stops with the usual message and the spool is deleted;
   - the installer copies the late file bytes from the kept chunks into their files and deletes each chunk once it has been used. Gap bytes are dropped.
5. **Pass 2** runs only if a dropped chunk held late file bytes. The installer restarts decompression from the checkpoint at or before the first dropped late byte, reads up to the end of the last one, and writes those bytes. Re-read bytes are checked against pass 1's per-interval CRC-32s (point 6). The member's CRC-32 still covers every byte, from pass 1.
6. **The cost is shown after pass 1.** A Deflate member's layout is only known once its tables have been decompressed, so the extra cost can't be shown before the install starts (the original "Directory-table order" section said it could). When pass 1 ends, the screen and `install.log` say how much pass 2 will re-read: MiB of the image, plus an estimated download at the compression ratio seen so far. The journal records the same numbers.
7. **Resume:** checkpoints work as before. The journal also records the kept chunks and `stop` (v1.3, specified in the MR that implements this). After a power cut, complete chunks are reused and the install resumes from the last checkpoint. A chunk after that checkpoint is rewritten.
8. Peak disk use is the game files written so far plus the spool. A game whose late data plus padding fits in the free space needs no pass 2 at all.

### Alternatives considered

- **A fixed spool size** (e.g. 1 GiB): it wastes disk that a big drive has, and can fail on a full one. The measurements show the right size varies by game, from 256 MiB to over 3 GiB.
- **Keeping the first unidentified bytes:** measured, and much worse (see above).
- **Re-downloading only (the original decision):** up to ~3.3 GB extra per game (D).
- **Two full passes:** worse than re-downloading only.
- **Converting the zip on a PC first:** excluded for the beta by [ADR 0010](0010-on-box-login-required-for-beta.md), since everything happens on the box.

### Consequences

- On the measured games, a spool of 1–2 GiB removes pass 2 for most of them. D still re-reads ~1.2 GiB with 2 GiB of spool.
- The installer needs the free-space hook (`xc_install_ops.free_space`) during the install, not just before it. For Deflate members, the free-space check moves to the point where every table is known.
- An unfinished install's folder can hold spool chunks. Their names are ones FATX accepts, and they are deleted when the install completes or is abandoned.
- The cost of a Deflate install is known only after pass 1. It is shown on screen and in the log, which is accepted for the beta.
