# 0009. Install from zip archives by streaming the zip itself

- **Status:** Proposed. Zip support is milestone M9b, listed as optional. This ADR proposes building it right after M5 (HTTPS); the owner decides the timing.
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

## Decision (proposed)

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
6. **Resume:** the journal saves the decompressor state at file boundaries: miniz's `tinfl` state plus its 32 KiB window, about 43 KiB, and the compressed offset. A resumed install restarts the download at that offset with `Range`.
7. **Library:** miniz (MIT; already planned in [ADR 0002](0002-license.md)), behind a small `core/zip` module with its own fuzz target, like every parser (HANDOFF §7.3).
8. **Maps:** `…zip/<member>` URLs (archive.org's on-the-fly extraction) are rejected when a map is loaded, with a message that points to the zip URL. The map spec gains this rule in the MR that adds zip support.

## Consequences

- The owner's sources become installable directly on the Xbox, with no PC step and no 1 GB stop.
- A full-disc zip install downloads the whole zip (5.74 GB for the tested item), versus about 7.8 GB uncompressed. At the hoped-for 1–3 MB/s that's roughly 30–95 minutes. Deflate is much faster than the network on the Xbox CPU, so it shouldn't be the bottleneck. M5 measures the network side.
- Memory: about 43 KiB of decompressor state, plus the existing I/O buffer.
- Some zips will be refused (Deflate64 from Windows' built-in zipper, LZMA from 7-Zip's zip mode). Each refusal names the method.
- Adds `core/zip`, miniz, a fuzz target, Zip64 test fixtures (the fixture generator gains zip output), and changes to the map spec and `extract` (a sequential-source mode).
