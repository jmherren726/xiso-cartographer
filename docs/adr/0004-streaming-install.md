# 0004. Install by stream-extracting the image over HTTP Range

- **Status:** Accepted; §4 amended by [ADR 0011](0011-checkpoints-inside-large-files.md) (resume inside large files from 64 MiB checkpoints)
- **Date:** 2026-10-13

## Context

A game install takes a disc image (`.xiso.iso`, or a full Redump-layout `.iso`) from an HTTP(S) server and ends up as a folder of game files under `<drive>:\Games\<title>\`. The constraints (HANDOFF §2):

- **FATX can't store a file of 4 GB or more.** Many images are larger, so downloading the image and then extracting it doesn't work. Splitting the image into parts would need twice the disk space on drives that are often small.
- **Every file inside an XDVDFS image is under 4 GB**, because the disc format uses a 32-bit size field. Each one can be written straight to FATX.
- **64 MB RAM.** Everything must stream through fixed buffers.
- **Range works on the target servers.** archive.org answers `Range` with `206 Partial Content` after one cross-host redirect, so we can read any byte range of a remote image.
- Downloads of several GB over a slow link will be interrupted, so installs must resume.

## Decision

1. **Stream-extract; never store the image.** Read the volume descriptor (sector 32, offset `0x10000`) and the directory tables with `Range` requests. Build the list of `(path, offset, size)`, sort it by offset, then fetch each file's bytes and write them straight into the destination file.
2. **Range is required.** A source that answers `200` (the whole body) to a `Range` request, or a `Content-Range` that doesn't match the request, is rejected before anything is written. The user gets a clear message saying the server doesn't support resumable downloads. There's no silent fallback.
3. **Coalesce ranges.** Neighbouring files are fetched in a single request when the gap between them is small. The threshold is a tunable constant (initially 1 MiB) and gets set from the M3 benchmarks. Bytes in the gaps are read and discarded.
4. **Journal for resume.** A small journal file in the install folder records each completed file (path, size and, when verifying, hash state). On resume, completed files are skipped, and the file that was in progress is truncated and written again from its start.
5. **Verification depends on the layout:**
   - **Trimmed xiso** (the target format): file data fills almost the whole image, so the installer reads the image **front to back in full** and treats gaps as "read and discard". Every byte passes through the image hash, so the whole-image MD5/SHA-1 from the source's metadata (archive.org) can be checked at the end. Hash state is saved in the journal at each file boundary, so resume doesn't break verification. BearSSL's hash API can export and restore its internal state.
   - **Full Redump layout:** the game partition sits after about 387 MB of video partition, and there may be large unused areas. Reading everything to compute the image hash would waste a lot of download, so these installs skip the gaps and verify **sizes only**. Per-file hashes can be added later if a source provides them.
   - Every install checks file sizes and that each written file has exactly the bytes requested.
6. **Free space is checked before writing**, using the total size calculated from the directory tables.

## Consequences

- No temporary image and no double disk space. Images over 4 GB install like any other.
- Installs survive cable pulls and power cuts. The cost is one journal write per completed file.
- **Servers without Range support can't be used.** That includes Python's `http.server`. The docs will point LAN users to a server that supports Range; the optional PC helper (M10) can be that server.
- Whole-image hash verification exists only for trimmed xisos, which are the v1 target format. Redump-layout installs get weaker verification, and the UI must say so.
- The xdvdfs reader and the extractor work through an abstract `ByteSource` (any byte range) and `Sink` (write file). The same code installs from a local ISO (M2), a LAN server (M4) and HTTPS (M5), and the host tests can feed it fixture images.
- The pass is sequential, at most one connection per install, which fits the "good guest" rule for archive.org ([ADR 0006](0006-no-links-no-bypass.md)).
