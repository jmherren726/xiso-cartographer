# 0011. Resume inside large files from checkpoints

- **Status:** Accepted (owner decision, 2026-10-15)
- **Date:** 2026-10-15
- **Amends:** [ADR 0004](0004-streaming-install.md) §4 ("the file that was in progress is truncated and written again from its start")

## Context

ADR 0004 resumes an interrupted install file by file: finished files are skipped, and the file that was being written starts again from byte 0. The first M4 hardware run showed what that means. The over-4 GB test image is almost all one 4,294,965,248-byte file, so a power cut at 1.3 GB lost all of it and the next start began from 0. Real games have single files of hundreds of MB to several GB too. At the console's ~8 MB/s, restarting a 4 GB file costs about nine minutes, and a run of short outages could keep a big file from ever finishing.

## Decision

1. **Checkpoints every 64 MiB inside a file.** When a file being written reaches each 64 MiB boundary, the installer writes out what it has buffered, flushes the file to disk (`NtFlushBuffersFile` on the Xbox), then appends a `part <bytes> <crc32-so-far> <path>` line to the journal ([journal.md](../formats/journal.md) §5, format v1.1) and flushes that too. Files of 64 MiB or less get no checkpoints; they are cheap to redo.
2. **Resuming continues from the last checkpoint.** If a file has a `part` line but no `done` line, and the file on disk is at least that long, the installer cuts it back to the checkpoint, carries on the CRC-32 from the recorded value, and asks the server for the rest of the file only. Otherwise it writes the file from the start, as before. At most 64 MiB is fetched again.
3. **Verification is unchanged.** The CRC-32 carried across the checkpoint covers the whole file, and the read-back check (journal.md §7) compares the whole file with it.
4. **Compatibility.** `part` lines come with journal format v1.1. A v1.0 reader skips them (they're a newer minor version's records) and redoes the file from the start, which is still correct.

## Consequences

- A power cut or long outage costs at most 64 MiB of download per interrupted file, instead of the whole file.
- One extra flush and journal line per 64 MiB written: a 4 GB file adds 64 lines (about 5 KB), and the flushes are negligible next to the data.
- Sinks gain two optional operations: making the open file durable (`sync`), and reopening a file cut back to a given length (`resume_file`). A sink without them simply gets no checkpoints.
- The 64 MiB interval is a constant (`XC_INSTALL_CHECKPOINT`); the request can lower it (tests use a few KiB).
