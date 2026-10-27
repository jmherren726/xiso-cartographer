# Settings file format (`xcat-settings`), version 1

**Status:** Normative. Implemented by `xcat/settings.h` (`core/src/settings.c`, M8, fuzzed), which the console reads at startup and writes from SETTINGS. The reasons behind it are in [ADR 0016](../adr/0016-settings-file-and-install-location.md).

The settings file keeps what the user chose in SETTINGS that isn't a login or a source: logins live in the [credentials file](credentials.md), and sources in the [map](map.md). Version 1 holds one setting, the install location.

The words **must**, **must not**, **should** and **may** are used as in RFC 2119.

## 1. Example

```
# xcat-settings v1

install_dir = F:\Games
```

## 2. Location and loading

- On the console: `E:\Apps\xcat\settings`, with no extension.
- The file is read once, at startup. The app writes it when the user changes a setting (§6), and the change applies at once.
- If the file is missing, every setting has its default (§4). That isn't an error.

## 3. Syntax

1. UTF-8 text, optional leading BOM ignored, LF or CRLF line endings, at most **16 KiB** in total and **1024 bytes** per line (without the line ending). A NUL or any other byte that isn't UTF-8 text is an error.
2. The first line that isn't blank **must** be the header `# xcat-settings v<MAJOR>[.<MINOR>]`. Versioning follows [map.md §3](map.md#3-header-and-versioning): a newer major version is rejected.
3. After it, leading and trailing whitespace is ignored on every line. Blank lines and lines starting with `#` are skipped. Every other line is `key = value`, split at the first `=`, with whitespace around both trimmed. A key is one or more of `a–z` and `_`, and the value isn't empty. Control characters (other than tab) aren't allowed.
4. Keys this version doesn't know are skipped, so a file written by a newer minor version still loads. A key it knows **must not** appear twice.

## 4. Keys

### 4.1 `install_dir`

The folder games are installed to. Default: `E:\Games`.

- It starts with a drive, `E:\`, `F:\` or `G:\` (the letter in either case; the app writes it in capitals). E: is the console's games partition; F: and G: are the extended partitions 6 and 7 that larger, modded drives have.
- After the drive come one or more folder names separated by single `\`, with no `\` at the end. Each name follows the FATX rules of [fatx-names.md](fatx-names.md): 1–42 bytes, only bytes the kernel accepts, and not `.` or `..`. A name **must not** end with a space: the file trims the end of a line (§3.3), so it would read back as another folder. The drive itself (`E:\`) isn't allowed.
- From the `\` after the drive, it is at most **64 bytes**, so a game's own paths still fit FATX's 250 bytes (ADR 0008 counts them from this folder).
- The folder is made if it isn't there. If it can't be made at startup (its drive is gone), the app installs to `E:\Games` for that run and says so.

Games already installed stay where they are when the setting changes. The library's INSTALLED section and the INSTALLED marks in GAMES look in the folder chosen now.

## 5. Errors

The file is **all-or-nothing**. Any problem rejects the whole file with its line number and a reason, and every setting keeps its default. Problems include an unreadable or oversized file, a missing or malformed header, a newer major version, a line too long or not text, a line that isn't `key = value`, a bad key name, an empty value, a known key given twice, and an `install_dir` that breaks §4.1.

The app shows the problem when the library opens, and doesn't write the file until it's fixed or deleted.

## 6. Writing the file

When the user changes a setting, the app writes `key = value` into the file:

- the **last** line that sets the key is replaced, with LF;
- if no line sets it, the line is added at the end (after a line ending, if the file has none);
- with no file, a new one: the header, a blank line, and the line.

Every other byte stays as it was, comments and unknown keys included. The file must parse before the change (§5), or nothing is written, and the result must parse too. On the console the file is replaced atomically, like the credentials file: written to `settings.tmp`, flushed, then renamed over `settings`.

## 7. Changes

| Version | Date | Change |
|---|---|---|
| 1.0 | 2026-10-27 | First version, with `install_dir`. |
