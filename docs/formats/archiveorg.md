# archive.org item metadata (`@archiveorg`)

**Status:** Normative. §1–§6 are implemented by `xcat/archiveorg.h` (M6), §7 by the map loader (`xcat/map.h`, `xcat-cli map`), and §8 by `xc_install` (`expect_size`, the expected hashes, the title ID from `xcat/xbe.h`) and the console. The reasons are in [ADR 0013](../adr/0013-archiveorg-source.md).

`@archiveorg <identifier>` in a map ([map.md §5.2](map.md#52-archiveorg-identifier)) turns the image files of one archive.org item into map entries. The item's listing comes from archive.org's public metadata API. It is input from the network, so every field is checked.

The words **must**, **must not**, **should** and **may** are used as in RFC 2119.

## 1. Identifier and request

- The identifier is 1–100 characters from `A–Z a–z 0–9 . _ -` ([map.md §5.2](map.md#52-archiveorg-identifier)).
- The listing is fetched with `GET https://archive.org/metadata/<identifier>`, like any other request ([http.md](http.md)), so credentials are attached only where [credentials.md §6](credentials.md#6-when-credentials-are-sent) allows. The listing is public, but a login doesn't hurt.
- The response body **must not** be larger than **4 MiB** (`XC_ARCHIVEORG_MAX_METADATA`). A larger one fails the directive. A collection of about 140 games is about 43 KB.

## 2. The response

The body **must** be one JSON value (§6). The reader looks at these parts of it and skips everything else:

```json
{
  "is_dark": true,
  "files": [
    { "name": "Game (USA).zip", "source": "original", "size": "6266337551",
      "md5": "<32 hex>", "sha1": "<40 hex>", "crc32": "<8 hex>", "private": "true" }
  ]
}
```

| Body | Result |
|---|---|
| Not JSON, or not an object | Error: "archive.org didn't send the metadata of item `<id>`" |
| `"is_dark": true` | Error: "archive.org item `<id>` was taken down" |
| No `files` member (e.g. `{}`) | Error: "archive.org item `<id>` doesn't exist or was removed" |
| `files` isn't an array | Error, as for "not JSON" |
| `files` is an array | The files are read as in §3. If none is chosen: warning "archive.org item `<id>` has no `.iso`, `.xiso` or `.zip` files" |

`is_dark` is checked first: a taken-down item may still list its files. Elements of `files` that aren't objects are skipped.

## 3. Choosing files

A file becomes an entry if all of these hold:

1. `name` is a string ending in `.iso`, `.xiso` or `.zip` (ASCII case-insensitive). `.xiso.iso` is covered by `.iso`.
2. `source` is the string `original`, or absent.
3. The name is valid (§3.1) and `size` is valid (§3.2).

A file that matches rules 1 and 2 but fails rule 3 is skipped and counted, and the map gets a warning naming the first one. Files that don't match rules 1 and 2 (thumbnails, torrents, metadata, derivatives) are skipped silently, and so is a `name` that isn't a string, doesn't decode (§6), or decodes to more than 2,048 bytes.

### 3.1 Name

- 1–512 bytes of valid UTF-8, with no control characters (below `0x20`, or `0x7F`) and no `\`.
- `/` separates folders within the item. The name **must not** start or end with `/`, contain `//`, or have a `.` or `..` segment.

### 3.2 Size

`size` is a string of decimal digits (as archive.org sends it) or a JSON integer, from 1 to 2^63 − 1, with no sign, fraction, exponent or leading zero. Without a valid size, the file is skipped.

### 3.3 Hashes

`md5` (32 hex digits), `sha1` (40) and `crc32` (8) are each optional, and any case is accepted. A hash that is present but malformed is dropped and counted, and the file is kept without it.

`private` (`"true"`) is recorded but changes nothing: whether a download needs a login is up to the server and the credentials file.

## 4. The entry

For each chosen file, in the listing's order:

| Field | Value |
|---|---|
| Name (the entry's key) | The last segment of the file name, without its extension: the longest of `.xiso.iso`, `.iso`, `.xiso`, `.zip` that matches (case-insensitive). `Game (USA).zip` → `Game (USA)`. If nothing is left (`.zip`), the file is skipped as invalid. |
| URL | `https://archive.org/download/<identifier>/` followed by the file name, percent-encoded: every byte except `A–Z a–z 0–9 - . _ ~ /` becomes `%XX` (upper-case hex). |
| Size, hashes | As in §3.2–§3.3 |
| Zip | Whether the name ends in `.zip`, as for any map URL ([map.md §4.2](map.md#42-url)) |

Generated entries have **no title ID**. The install reads it from the game's `default.xbe` (§8), and the catalog matches names to title IDs from M7 ([ADR 0013](../adr/0013-archiveorg-source.md) decision 1).

## 5. Limits

| Limit | Value | When exceeded |
|---|---|---|
| Response body | 4 MiB | Directive fails |
| JSON nesting | 32 levels | Directive fails, as for "not JSON" |
| Files chosen per item | 10,000 (the map's own limit, [map.md §6](map.md#6-limits)) | The map's rule applies |

## 6. JSON subset

The reader accepts [RFC 8259](https://www.rfc-editor.org/rfc/rfc8259) JSON, strictly:

- one value, with only whitespace (space, tab, LF, CR) around it;
- strings in valid UTF-8 with no raw control characters, and only the escapes `\" \\ \/ \b \f \n \r \t \uXXXX`. In strings the reader uses, a `\u` surrogate must be part of a correct pair, and `\u0000` makes the string invalid;
- numbers by RFC 8259's grammar (no leading zeros, `+`, `NaN`, hex or trailing `.`);
- `true`, `false`, `null`;
- nesting at most 32 deep, with no trailing commas and no comments.

Anything else makes the whole body invalid. Strings and numbers in parts the reader doesn't use are checked for syntax, but not decoded.

## 7. In the map

The directive's entries are inserted at its position, so [map.md §4.3](map.md#43-duplicate-keys) applies: a later entry with the same name (ASCII case-insensitive) replaces an earlier one, with a warning. Errors from §2 are line errors of the directive's line. Each generated URL is also checked like a written one ([map.md §4.2](map.md#42-url)): a file in a folder whose name ends in `.zip` would be read as a file inside a zip, so it is skipped with a warning. Warnings name the item and count what was skipped: files with a bad name or size (naming the first), URLs refused, malformed hashes. The console lists generated entries without installing them until choosing what to install comes later in M6.

## 8. Installing a generated entry

- Before anything is written, the size of the file on the server (from its `Content-Range`) **must** equal the listing's size. Otherwise the install fails with "The file isn't the size its listing gives (a different file?)" (`XC_ERR_SOURCE_SIZE`), and the console logs both sizes.
- For an `.iso` or `.xiso` file, the listing's MD5 and SHA-1 are passed on as the expected hashes. They are compared when the image is a trimmed xiso, which is hashed whole ([journal.md §7](journal.md#7-verification)), like `xcat-cli install --md5/--sha1`. A full-disc `.iso` skips its video partition, so it isn't hashed whole, and its hashes go unused.
- A zip's MD5, SHA-1 and CRC-32 cover the zip, not the image, so they aren't passed on. The member's CRC-32 covers every byte of the image ([zip.md §9](zip.md)).
- After verification, the title ID and name from the installed `default.xbe` ([xbe.md](xbe.md)) are reported and written to the marker ([journal.md §7.1](journal.md#71-the-marker)).

## 9. Changes

| Version | Date | Change |
|---|---|---|
| 1 | 2026-10-20 | First version (M6): §1–§6 implemented. |
