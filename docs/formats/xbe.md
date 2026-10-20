# XBE certificate (title ID and name)

**Status:** Normative. Implemented by `xcat/xbe.h` (M6). After an install, the installer reads the game's title ID from its `default.xbe` and records it ([ADR 0013](../adr/0013-archiveorg-source.md) decision 1, [journal.md §7.1](journal.md#71-the-marker)). This lets entries keyed by name ([archiveorg.md](archiveorg.md)) be tied to a title ID later.

An XBE is the Xbox's executable format. Only the few header fields needed to find the certificate are read; nothing is executed or loaded. All integers are little-endian. The file comes from a downloaded image, so it is untrusted.

The words **must**, **must not**, **should** and **may** are used as in RFC 2119.

## 1. Image header

| Offset | Size | Field |
|---|---|---|
| `0x000` | 4 | magic `XBEH` |
| `0x004` | 256 | signature (not checked) |
| `0x104` | 4 | base address: where the headers are loaded in memory |
| `0x108` | 4 | size of all headers, in bytes |
| `0x118` | 4 | certificate address (in memory) |

## 2. Certificate

The certificate sits at file offset `certificate address − base address`.

| Offset | Size | Field |
|---|---|---|
| `0x000` | 4 | size of the certificate |
| `0x004` | 4 | time stamp |
| `0x008` | 4 | **title ID**, e.g. `0x4D530004`, the same number map keys use ([map.md §4.1](map.md#41-key)) |
| `0x00C` | 80 | **title name**, 40 UTF-16LE code units, ended by a 0 unit if shorter |

## 3. Checks

The reader refuses the file ("not an XBE") unless all of these hold:

1. The file is at least `0x178` bytes and starts with `XBEH`.
2. `size of headers` is at least `0x178` and no larger than the file.
3. `certificate address ≥ base address`, and the certificate's offset is at least `0x178`.
4. The certificate's first `0x5C` bytes lie inside the headers (`offset + 0x5C ≤ size of headers`), and its `size` field is at least `0x5C`.

The title name is converted to UTF-8. A unit in `0xD800`–`0xDFFF` (UTF-16 surrogates; a valid pair is two of them) and a control character (below `0x20`, or `0x7F`–`0x9F`) each become `?`. Trailing spaces are trimmed. An empty name is allowed.

## 4. Changes

| Version | Date | Change |
|---|---|---|
| 1 | 2026-10-20 | First version (M6). |
