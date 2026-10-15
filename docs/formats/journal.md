# Install journal format (`xcat-journal`), version 1

**Status:** Normative. Implemented by `xcat/journal.h` (M4). The reasons behind it are in [ADR 0004](../adr/0004-streaming-install.md) §4: installs must survive cable pulls and power cuts, and resume without downloading finished files again.

The journal is a small text file in the install folder. It says which image the install comes from and which files are already complete. The installer appends one line per finished file and deletes the journal when the install is complete and verified. No user ever edits it.

The words **must**, **must not**, **should** and **may** are used as in RFC 2119.

## 1. Example

```
# xcat-journal v1 *5B6A2C1E
image 4610330624 0 2D84F0A7 21 *0C59D2B1
validators "5e1f-61a2c3"  Wed,%2014%20Oct%202026%2010:00:00%20GMT *A33E1F09
source http://192.168.1.20:8080/xbox/Game%20(Europe).xiso.iso *7F10B2C4
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
5. Numbers are decimal without leading zeros, except checksums and digests, which are 8 upper-case hex digits.

## 4. Identity

The lines right after the header identify the image. They **must** appear in this order:

| Line | Fields | Meaning |
|---|---|---|
| `image` | `<size> <partition-offset> <listing-crc> <file-count>` | Image size in bytes; the game partition's offset ([xdvdfs.md](xdvdfs.md)); the listing digest (below); the number of files |
| `validators` | `<etag> <last-modified>` | The server's `ETag` and `Last-Modified` from the first response ([http.md §7.3](http.md#73-resume)), percent-encoded; `-` for one the server didn't send |
| `source` | `<url>` | The URL the install started from, percent-encoded. For messages only; it never decides anything |

**Listing digest:** the CRC-32 over, for every file in listing order (offset, then path), the bytes of `<offset> <size> <path>\n`, with the numbers in decimal and the path as stored in the listing ('/'-separated, not encoded). Two images with the same digest have the same files at the same offsets with the same sizes.

The journal **matches** an image when the image size, partition offset, listing digest and file count are equal, and each validator is equal where both the journal and the server have one. Otherwise it belongs to a different image (§6).

## 5. Records

```
done <size> <crc32> <path>
```

- `size` is the file's size and `crc32` the CRC-32 of its content, as received and written.
- `path` is the file's listing path, percent-encoded.
- Each file has at most one `done` line. A reader that finds a second one for the same path treats the journal as invalid (§6).

Later minor versions may add record types (for example hash state for whole-image verification, M5).

## 6. Reading a journal

1. Read the lines in order and stop at the first torn line (§3.2): it and everything after it are ignored. That's what a power cut during an append leaves behind.
2. If the header is missing or malformed, the major version is newer than the reader's, or the identity lines are missing, torn or malformed, the journal is **invalid**.
3. A `done` line that is malformed, names a path that isn't in the image's listing, or gives a size that doesn't match the listing makes the journal **invalid**. These can't come from a power cut, so something else wrote or damaged the file.
4. **Invalid, or not matching (§4):** the folder belongs to another image or can't be trusted. The installer **must not** reuse or delete its files; it installs into a new folder instead, named as for an existing folder ([ADR 0008](../adr/0008-refuse-games-fatx-cant-store.md)), and says so.
5. **Valid and matching:** the `done` files are skipped. Before trusting one, the installer checks that the file exists with the recorded size; if it doesn't, the file is written again.

## 7. Verification

When every file is done, the installer reads each one back from the disk and compares its CRC-32 with its `done` line, including files written before a power cut. Any difference fails the install and keeps the journal, so a later attempt can rewrite that file. Whole-image hashes ([ADR 0004](../adr/0004-streaming-install.md) §5) come with M5.

## 8. Changes

| Version | Date | Change |
|---|---|---|
| 1.0 | 2026-10-15 | First version. |
