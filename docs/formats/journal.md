# Install journal format (`xcat-journal`), version 1.3

**Status:** Normative. Implemented by `xcat/journal.h` (format, reading) and `xcat/install.h` (the life cycle of §2, §6 and §7), M4; `hash` records and whole-image hashes (`xcat/hash.h`) since M5; the `zip` identity (§4.1, §6.8) since M5b. The reasons behind it are in [ADR 0004](../adr/0004-streaming-install.md) §4: installs must survive cable pulls and power cuts, and resume without downloading finished files again.

The journal is a small text file in the install folder. It says which image the install comes from and which files are already complete. The installer appends one line per finished file and deletes the journal when the install is complete and verified. No user ever edits it.

The words **must**, **must not**, **should** and **may** are used as in RFC 2119.

## 1. Example

```
# xcat-journal v1.3 *5B6A2C1E
image 4610330624 0 2D84F0A7 21 *0C59D2B1
validators "5e1f-61a2c3"  Wed,%2014%20Oct%202026%2010:00:00%20GMT *A33E1F09
source http://192.168.1.20:8080/xbox/Game%20(Europe).xiso.iso *7F10B2C4
part 67108864 3C1A88E2 data/big.dat *2A09BC17
part 134217728 0B7F4D10 data/big.dat *61C3E5A2
hash 134283264 0123456789ABCDEF0123456789ABCDEF 0123456789ABCDEF0123456789ABCDEF01234567 *4F02D6B9
done 4294965248 9A0C3B71 data/big.dat *E1D24A80
done 1 5E1E4C5F a%20b/one-byte.bin *3BC0A912
```

(The CRCs above are illustrative.)

## 2. Location and life cycle

1. The journal is `<install folder>\.xcat-journal` (FATX allows the name, [fatx-names.md](fatx-names.md)).
2. **Starting an install:** the installer creates the install folder and writes the header and the identity lines (§4) **before** any game file.
3. **After each file:** the installer writes the file's data, closes the file and **flushes it to disk** (on the Xbox, `NtFlushBuffersFile`), then appends a `done` line (§5) and flushes the journal. A `done` line therefore always describes data that is on disk.
4. **Finishing:** after verification (§7), the installer writes the `.xcat-installed` marker, then deletes the journal. A folder with a journal and no marker is an unfinished install.
5. **Resuming:** the installer reads the journal (§6). If its identity matches the image about to be installed, files with a valid `done` line are skipped, and every other file, including one that was half-written when the power went, is written again from its start.

## 3. Syntax

1. ASCII text, LF line endings (a CR before the LF is tolerated). At most **4 MiB** in total and **8192 bytes** per line.
2. Every line ends with ` *` and 8 upper-case hex digits: the CRC-32 ([`xcat/crc32.h`](../../core/include/xcat/crc32.h), zlib variant) of every byte of the line before the ` *`. A line whose CRC doesn't match, or that has no line ending, is **torn**.
3. The first line is the header `# xcat-journal v<MAJOR>[.<MINOR>]` (with its CRC). Versioning follows [map.md §3](map.md#3-header-and-versioning): a newer major version is not read (§6), and a reader ignores record types it doesn't know if the minor version is newer than its own.
4. Fields are separated by single spaces. Strings that could contain spaces or other special bytes are **percent-encoded**: every byte below `0x21`, above `0x7E`, and `%` itself is written as `%XX` (upper-case hex). Everything else is written as is.
5. Numbers are decimal without leading zeros, except CRC-32 checksums, which are 8 upper-case hex digits, and hash states (§5), which are upper-case hex of their fixed length.

## 4. Identity

The lines right after the header identify the image. They **must** appear in this order:

| Line | Fields | Meaning |
|---|---|---|
| `image` | `<size> <partition-offset> <listing-crc> <file-count>` | Image size in bytes; the game partition's offset ([xdvdfs.md](xdvdfs.md)); the listing digest (below); the number of files |
| `validators` | `<etag> <last-modified>` | The server's `ETag` and `Last-Modified` from the first response ([http.md §7.3](http.md#73-resume)), percent-encoded; `-` for one the server didn't send |
| `source` | `<url>` | The URL the install started from, percent-encoded. For messages only; it never decides anything |

**Listing digest:** the CRC-32 over, for every file in listing order (offset, then path), the bytes of `<offset> <size> <path>\n`, with the numbers in decimal and the path as stored in the listing ('/'-separated, not encoded). Two images with the same digest have the same files at the same offsets with the same sizes.

The journal **matches** an image when the image size, partition offset, listing digest and file count are equal, and each validator is equal where both the journal and the server have one. Otherwise it belongs to a different image (§6).

### 4.1 Deflate zip members (v1.3)

An image read from a Deflate zip member ([stream-install.md](stream-install.md)) has no listing when its install starts: the directory tables arrive with the stream. Its journal has a `zip` line in place of the `image` line:

| Line | Fields | Meaning |
|---|---|---|
| `zip` | `<size> <member-crc32> <compressed-size>` | The member's uncompressed size, CRC-32 and compressed size, from the zip's central directory ([zip.md §4](zip.md#4-central-directory)) |

It matches when those three values are equal and the validators match as above. A `zip` journal never matches an `image` one. A stored member has its listing up front, so its journal has an `image` line like a bare image's.

## 5. Records

```
done <size> <crc32> <path>
part <bytes> <crc32> <path>            (v1.1)
hash <offset> <md5-state> <sha1-state> (v1.2)
```

- `done`: the whole file is on disk. `size` is the file's size and `crc32` the CRC-32 of its content, as received and written.
- `part` ([ADR 0011](../adr/0011-checkpoints-inside-large-files.md)): a checkpoint inside a file still being written. Its first `bytes` bytes are on disk (the file was flushed before this line), and `crc32` is the CRC-32 of exactly those bytes. `bytes` is greater than 0 and less than the file's size. Installers write one every 64 MiB of a large file.
- `path` is the file's listing path, percent-encoded.
- Each file has at most one `done` line, and no `part` line after its `done` line. If a file has several `part` lines, the last one counts. (They can go down: a file that couldn't continue from its checkpoint is written again from the start, with new, smaller checkpoints.) A reader that finds anything else treats the journal as invalid (§6).

- `hash` ([ADR 0004](../adr/0004-streaming-install.md) §5): the whole-image MD5 and SHA-1 so far. Every image byte in `[0, offset)` has gone through both hashes, in order. `offset` is a multiple of **64** (the hashes' block size) and at most the image size. `md5-state` is MD5's 16-byte chaining value (32 hex digits) and `sha1-state` SHA-1's 20-byte one (40 hex digits), each as BearSSL's `br_md5_state` / `br_sha1_state` write it (MD5's words little-endian, SHA-1's big-endian). The last `hash` line counts.
- Only installs that hash the image write `hash` lines: trimmed xisos (§7). The installer writes one after a `done` or `part` line once the image has advanced at least **64 MiB** past the previous `hash` line, so a resume re-reads at most that much plus the current file. Its `offset` is then the position the install has read to, rounded down to a multiple of 64.

Later minor versions may add other record types.

## 6. Reading a journal

1. Read the lines in order and stop at the first torn line (§3.2): it and everything after it are ignored. That's what a power cut during an append leaves behind.
2. If the header is missing or malformed, the major version is newer than the reader's, or the identity lines are missing, torn or malformed, the journal is **invalid**.
3. A `done`, `part` or `hash` line that is malformed (for `hash`: an offset that isn't a multiple of 64 or is past the image's end, or a state of the wrong length), names a path that isn't in the image's listing, gives a size that doesn't match the listing (`done`) or a byte count of 0 or at least the file's size (`part`), or comes after the file's `done` line makes the journal **invalid**. These can't come from a power cut, so something else wrote or damaged the file.
4. **Invalid, or not matching (§4):** the folder belongs to another image or can't be trusted. The installer **must not** reuse or delete its files; it installs into a new folder instead, named as for an existing folder ([ADR 0008](../adr/0008-refuse-games-fatx-cant-store.md)), and says so.
5. **Valid and matching:** the `done` files are skipped. Before trusting one, the installer checks that the file exists with the recorded size; if it doesn't, the file is written again.
6. A file with `part` lines but no `done` line resumes at its last checkpoint, if the file on disk is at least that long: the installer cuts it back to `bytes`, continues the CRC-32 from `crc32`, and downloads only the rest. Otherwise the file is written again from the start. A v1.0 reader skips `part` lines (§3.3), which also just rewrites the file.
7. **Hashes (v1.2).** The installer restores both hashes from the last `hash` line and reads the image on from its `offset`, discarding the bytes of files that are already done. That is only possible when no file still to be written (or continued, §6.6) starts before that offset. If one does (a `done` file was missing, §6.5), or the journal has no `hash` line (a v1.0 or v1.1 journal), hashing starts again from the image's first byte: the files already done are read again, but not written.
8. **Zip journals (v1.3, §4.1).** They are read before the listing is known: each `done` line is kept by path, and is checked when the file becomes known. The path must be in the listing, and the size must match, or the line doesn't count and the file is written again. The file must also still be on disk with that size (§6.5). Two `done` lines for one path, a malformed record, or any other record type make the journal invalid. Their install restarts the stream from the start (resuming in the middle of the stream comes later), so a power cut costs the download so far, but no finished file.

## 7. Verification

When every file is done, the installer reads each one back from the disk and compares its CRC-32 with its `done` line, including files written before a power cut. Any difference fails the install and keeps the journal, so a later attempt can rewrite that file.

**Whole-image hashes** ([ADR 0004](../adr/0004-streaming-install.md) §5, M5). For a **trimmed xiso** (game partition at offset 0) the installer reads the image front to back in full, from its first byte to its last, gaps and directory tables included, and every byte goes through MD5 and SHA-1. The result is the hash of the image file as the server has it. When an expected MD5 or SHA-1 is given (`xcat-cli install --md5/--sha1`; from M6 the archive.org metadata), a mismatch fails the install with "The downloaded image doesn't match its published checksum", no marker is written and the journal is kept. Otherwise both values are reported. Full-disc layouts aren't hashed: reading their video partition and unused areas would cost too much download, so they're checked by size and CRC-32 only.

If files overlap in the image (two entries sharing data), a single front-to-back read can't feed every file. The install still completes, but it reports the image as not hashed.

## 8. Changes

| Version | Date | Change |
|---|---|---|
| 1.0 | 2026-10-15 | First version. |
| 1.1 | 2026-10-15 | `part` checkpoint records inside large files ([ADR 0011](../adr/0011-checkpoints-inside-large-files.md)); §6.6. |
| 1.2 | 2026-10-17 | `hash` records: whole-image MD5 and SHA-1 state for trimmed xisos ([ADR 0004](../adr/0004-streaming-install.md) §5, ADR 0012); §6.7 and §7. |
| 1.3 | 2026-10-20 | `zip` identity for Deflate zip members ([ADR 0009](../adr/0009-install-from-zip-archives.md)); §4.1 and §6.8. |
