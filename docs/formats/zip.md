# Zip archives (reading the directory and choosing the image)

**Status:** Normative. §1–§7 are implemented by `xcat/zip.h` (M5b) and used by `xcat-cli zip`. Streaming a member's data (stored or Deflate) comes in a later M5b MR, which adds its section here. The reasons are in [ADR 0009](../adr/0009-install-from-zip-archives.md).

The installer reads a zip with HTTP `Range` like any other source ([ADR 0004](../adr/0004-streaming-install.md)): first the end of the file, then the central directory, then the chosen member's local header. This document fixes the subset of [PKWARE's APPNOTE 6.3.10](https://pkware.cachefly.net/webdocs/casestudies/APPNOTE.TXT) that it accepts, and every check. Anything outside it is refused with a clear reason; nothing is guessed.

The words **must**, **must not**, **should** and **may** are used as in RFC 2119. All integers are little-endian; offsets are from the start of the zip file.

## 1. Overview

```
[local header 1][data 1] ... [local header n][data n]
[central directory: one header per entry]
[Zip64 end record][Zip64 locator]   (only in Zip64 archives)
[end of central directory record][comment]
```

The **central directory** is authoritative: names, methods, sizes and CRC-32s come from it. A local header is only read for the chosen member, to find where its data starts (§6).

## 2. End of central directory record (EOCD)

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | signature `0x06054b50` (`PK\5\6`) |
| 4 | 2 | number of this disk |
| 6 | 2 | disk where the central directory starts |
| 8 | 2 | entries on this disk |
| 10 | 2 | entries in total |
| 12 | 4 | central directory size |
| 16 | 4 | central directory offset |
| 20 | 2 | comment length `k` |
| 22 | `k` | comment |

1. The reader reads the last `min(size, 20 + 22 + 65,535)` bytes of the file in one request: the longest EOCD, plus room for a Zip64 locator before it (§3). It searches backwards from `size − 22` for the signature. The first candidate whose comment length ends exactly at the end of the file (`pos + 22 + k == size`) is the EOCD.
2. No candidate, or a file shorter than 22 bytes: **not a zip** (`XC_ERR_NOT_ZIP`).
3. Both disk numbers must be 0 and the two entry counts equal. Otherwise the zip is split or spanned: **unsupported** (`XC_ERR_ZIP_UNSUPPORTED`).

## 3. Zip64

| Zip64 end locator (20 bytes, right before the EOCD) | | |
|---|---|---|
| 0 | 4 | signature `0x07064b50` |
| 4 | 4 | disk with the Zip64 end record |
| 8 | 8 | Zip64 end record offset |
| 16 | 4 | total number of disks |

| Zip64 end record | | |
|---|---|---|
| 0 | 4 | signature `0x06064b50` |
| 4 | 8 | size of the rest of the record (at least 44) |
| 12 | 2 | version made by |
| 14 | 2 | version needed |
| 16 | 4 | number of this disk |
| 20 | 4 | disk where the central directory starts |
| 24 | 8 | entries on this disk |
| 32 | 8 | entries in total |
| 40 | 8 | central directory size |
| 48 | 8 | central directory offset |

1. If the 20 bytes before the EOCD start with the locator signature, the archive is **Zip64**:
   - the locator's disk must be 0 and its disk count 1 (otherwise unsupported);
   - the Zip64 end record must lie wholly before the locator, start with its signature, and have a rest size of at least 44;
   - its disk numbers must be 0 and its entry counts equal (otherwise unsupported);
   - its entry count, size and offset **replace** the EOCD's.
2. Without a locator, an EOCD field holding its all-ones value (`0xFFFF` or `0xFFFFFFFF`) means a Zip64 record is missing: **corrupt** (`XC_ERR_CORRUPT`).
3. The **end of the directory area** is where the Zip64 end record starts (Zip64) or where the EOCD starts (otherwise).

## 4. Central directory

1. The central directory must lie inside `[0, end of the directory area)`: `offset + size ≤ end`. Data before the first local header (a self-extractor stub) shifts every offset and isn't supported: offsets that don't fit are corrupt.
2. Its size must be at most **4 MiB**, and the entry count at most **16,384** (`XC_ERR_LIMIT`). The reader reads it in one request.
3. The reader parses exactly `entries` headers, one after another from the start. Bytes after the last one (e.g. a digital signature record) are ignored. Each header:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | signature `0x02014b50` |
| 4 | 2 | version made by |
| 6 | 2 | version needed |
| 8 | 2 | flags |
| 10 | 2 | method |
| 12 | 4 | DOS time and date |
| 16 | 4 | CRC-32 |
| 20 | 4 | compressed size |
| 24 | 4 | uncompressed size |
| 28 | 2 | name length `n` |
| 30 | 2 | extra length `m` |
| 32 | 2 | comment length `k` |
| 34 | 2 | disk number start |
| 36 | 2 | internal attributes |
| 38 | 4 | external attributes |
| 42 | 4 | local header offset |
| 46 | `n` + `m` + `k` | name, extra fields, comment |

4. A header whose signature is wrong, or which doesn't fit in the directory, is corrupt.
5. **Names:** 1 to 65,535 bytes and no NUL byte, otherwise corrupt. They're kept as bytes; flag bit 11 (UTF-8) only affects display. A name ending in `/` is a directory.
6. **Extra fields** are a sequence of `id` (2 bytes), `size` (2 bytes) and `size` bytes of data, which must exactly fill the `m` bytes (otherwise corrupt). Only the Zip64 field is read; others are skipped.
7. **Zip64 extended information** (`id 0x0001`): it holds, in this order, only the values whose header field is all ones: uncompressed size (8), compressed size (8), local header offset (8), disk number start (4). Its data must be at least as long as those values (extra bytes after them are ignored). A header field that is all ones without a Zip64 field to replace it is corrupt.
8. The disk number start must be 0 (otherwise unsupported).
9. Each entry's local header offset must be below the central directory offset (otherwise corrupt).

## 5. Choosing the image

1. The image is the one entry that isn't a directory and whose name ends in `.iso` or `.xiso`, compared without case for ASCII letters. A `.xiso.iso` name ends in `.iso`.
2. No such entry, or more than one: the install is refused (`XC_ERR_ZIP_MEMBER`), and every entry is listed so the user can see why. A map syntax for choosing one of several may come later (ADR 0009 §3).

## 6. The chosen member

1. **Supported:** method **0** (stored) or **8** (Deflate) ([ADR 0009 §2](../adr/0009-install-from-zip-archives.md)). Any other method is unsupported, and the message names it: 9 is Deflate64 (Windows' built-in zipper), 14 is LZMA (7-Zip), 99 is AES encryption.
2. Flag bits 0 (encrypted), 6 (strong encryption) and 13 (encrypted central directory) make the member unsupported. Bit 3 (sizes in a data descriptor after the data) is fine: the central directory has the sizes. Bits 1–2 (Deflate level) are ignored.
3. A stored member's compressed and uncompressed sizes must be equal (otherwise corrupt).
4. **Local header** (30 bytes, then name and extra) at the entry's local header offset:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | signature `0x04034b50` |
| 4 | 2 | version needed |
| 6 | 2 | flags |
| 8 | 2 | method |
| 10 | 4 | DOS time and date |
| 14 | 4 | CRC-32 |
| 18 | 4 | compressed size |
| 22 | 4 | uncompressed size |
| 26 | 2 | name length `n` |
| 28 | 2 | extra length `m` |

   - The signature, the method and the name bytes must match the central directory entry (otherwise corrupt).
   - Its CRC-32, sizes and extra fields aren't used. They may be zero, all ones, or Zip64 values, and may disagree when bit 3 is set.
   - The member's data starts at `local offset + 30 + n + m`, and must end at or before the central directory offset (otherwise corrupt).

## 7. Result

`xc_zip_list()` returns every entry (name, method, flags, CRC-32, both sizes, local header offset). `xc_zip_pick_image()` applies §5, and `xc_zip_open_member()` applies §6 and returns the data offset. The CRC-32 is checked when the member's data is read (later MR).

## 8. Security

Every offset and length comes from the file, which may be hostile. All reads go through bounds-checked helpers, sums are checked for overflow before use, and the reader allocates at most the 4 MiB directory plus one name per entry. `fuzz_zip` exercises §2–§6 on memory sources (CONTRIBUTING.md#fuzzing).
