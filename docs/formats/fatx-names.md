# FATX names and paths: the rules `core/fatx.c` enforces

**Status:** Normative for `core/src/fatx.c` (`xcat/fatx.h`). FATX is Microsoft's filesystem for the Xbox hard disk. This document records what the console's kernel accepts, as **measured** by [`app/fatx-probe`](../../app/fatx-probe/main.c), and the install checks built on it ([ADR 0008](../adr/0008-refuse-games-fatx-cant-store.md)).

**Source of truth:** no complete, authoritative list of FATX naming rules is published. Third-party references agree on the limits but not on the characters. So the probe created files directly through the kernel (`NtCreateFile`) and recorded what it accepted:

- **xemu v0.8.136**, kernel **1.0.4627.1** (the owner's BIOS), probe v2;
- **the owner's console**, kernel **1.0.5838.1**, probe v4 (2026-10-15, full report). It agrees on every limit, and adds the file-size limit (§4b). It accepts more name bytes (§2).

The full results are in [testing.md](../testing.md#fatx-probe-m2). If a later kernel or a probe run disagrees, update the table in `fatx.c`, its test, and this document together.

## 1. Names

- **1 to 42 bytes.** 42 is accepted, 43 is rejected, for files and directories alike.
- `.` and `..` are path components, not names.
- **Case-insensitive.** Lookups ignore case, and a name differing from an existing one only in case is rejected as a duplicate. The XDVDFS reader already rejects such pairs within one directory ([xdvdfs.md §4](xdvdfs.md#4-names-and-paths)), folding `a`–`z`. How the kernel folds bytes of 0x80 and above wasn't measured.
- **Edge cases are accepted:** a trailing dot, a trailing space, a leading space, `a..b`, `...`, `CON`, `nul.txt`.

## 2. Bytes

A name may contain any byte the kernel accepts. The kernel accepted 171 of the 254 bytes tested (0x00 can't occur in a name, and 0x5C `\` is the path separator; both are rejected).

| Rejected | Bytes |
|---|---|
| Control | `0x01`–`0x1F` |
| ASCII | `"` `*` `+` `,` `/` `:` `;` `<` `=` `>` `?` `\` `\|` (0x22 0x2A 0x2B 0x2C 0x2F 0x3A 0x3B 0x3C 0x3D 0x3E 0x3F 0x5C 0x7C) |
| ≥ 0x80 | 84 86 87 89 8B 8F 91 9F A0 A1 A2 A5 A6 A9 AA AD AE B0 BF C0 C1 C2 C5 C6 C9 CA CD CE D0 DF E0 E1 E4 E5 E6 E7 E9 EF F1 FF |

Everything else is accepted, including space, `!#$%&'()-.@[]^_` and backtick `{}~`, 0x7F, and the remaining bytes ≥ 0x80. The high-byte pattern has no obvious rule (it's probably the kernel's character table), so it's encoded as measured, not derived. `test_fatx.c` compares the code's table with this list byte for byte.

**Kernels differ on high bytes.** Kernel 1.0.5838.1 (the owner's console, probe v4) accepts **every** byte ≥ 0x80, including the 40 above, and agrees with kernel 1.0.4627.1 on everything below 0x80. A hard disk can move between consoles, so the code keeps rejecting the 40 bytes: the union of what any measured kernel rejects. Game file names are plain ASCII in practice, so this refuses nothing real.

## 3. Paths

- **At most 250 bytes**, counted from the first `\` after the drive. For example, `\Games\Halo\default.xbe` is 23. Directories of exactly 250 were created, and 251 was rejected.
- An installed file's path is `\Games\<folder>\` + its path in the image, with `/` written as `\`. So a game's longest path must fit in `250 − len("\Games\<folder>")`, and the folder name takes up to 42 of that.
- nxdk's own file calls (`CreateFileA`, `FindFirstFileA`) allow paths below 260 bytes including `E:`, which is looser than FATX's 250. The app may use them, provided it enforces 250 itself.

## 4. Directories

- **At most 4,096 entries** (files plus subdirectories) per directory. The 4,097th was rejected.

## 4b. File size

- **At most 4,294,950,912 bytes per file on E:** (4 GiB − 16 KiB, i.e. 4 GiB minus one 16 KiB cluster): `XC_FATX_MAX_FILE_SIZE`. Measured on hardware with probe v4: a file grows by writing to exactly that size, and every larger size fails as soon as the write crosses it, with `0xC0000102` (`STATUS_FILE_CORRUPT_ERROR`). Setting a larger end of file directly "succeeds", but the file can't actually be written that far.
- Found when the M4 install of a 4,294,965,248-byte test file failed in its last 16 KiB on the console, while the same install works on a PC.
- XDVDFS allows files up to 4 GiB − 1, so an image can contain a file FATX can't store. Such a game is refused like any other §5 problem. Partitions with bigger clusters (some F:/G: setups) would have a lower limit (2^32 − cluster); installs only go to E: today.

## 5. Installing a game

1. **The game's files keep their names.** A game opens its files by name, so renaming one breaks it. If any entry of the image breaks §1–§4b under the chosen install folder, the install is **refused** (`XC_ERR_NOT_INSTALLABLE`), and every problem is listed (`xc_fatx_check_listing`). Nothing is written.
2. **The install folder name is ours to choose,** and is made valid by `xc_fatx_title_name()`:
   - `": "` becomes `" - "` (so "Halo: Combat Evolved" becomes "Halo - Combat Evolved");
   - each run of other bytes that aren't printable ASCII accepted by §2 becomes one `_`;
   - leading and trailing spaces, dots and `_` are trimmed, and an empty result becomes `Game`;
   - the result is cut to 42 bytes, or less when the caller needs room.

   High bytes are replaced even where FATX accepts them. Titles are usually UTF-8, and dashboards don't display UTF-8 FATX names correctly.
3. **Taken folder names** get a numbered variant, `xc_fatx_numbered_name()`: "Title (2)", shortened so it still fits 42 bytes.

`xcat-cli check <image> [title]` runs the same check on a PC.
