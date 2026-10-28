# XBE title image (`$$XTIMAGE`)

**Status:** Normative for what the build writes. Implemented by [`tools/ui/xbe_title_image.py`](../../tools/ui/xbe_title_image.py) (M9), tested by `ui_xbe_title_image` (CTest). The app's own `default.xbe` gets its logo this way; nothing here is read from other XBEs.

Dashboards such as UnleashX and XBMC show an XBE's **title image**, which the XBE carries as a section named `$$XTIMAGE` holding an XPR0 texture. nxdk's `cxbe` makes the XBE from the linker's output and can only set the small boot logo, not add this section. So the build adds it to `default.xbe` after `cxbe` ([app/Makefile](../../app/Makefile)). The image is the project's logo, `app/assets/title-image.png` ([its README](../../app/assets/README.md)).

There's no public specification of the section. What follows is the layout the dashboards and other XBE tools read, as written here and checked against an independent reader ([pyxbe](https://github.com/mborgerson/pyxbe), which finds and decodes the section). All integers are little-endian.

The words **must**, **must not**, **should** and **may** are used as in RFC 2119.

## 1. The image

- 128 × 128 pixels, from an 8-bit RGB or RGBA PNG; alpha is dropped. Any other size is refused.
- DXT1 (BC1), in its four-colour mode only (no transparent pixels): each 4 × 4 block is 8 bytes, the two RGB565 end colours (`color0` greater than `color1`) and sixteen 2-bit indices, the first pixel in the low bits, row by row. A block of one colour has `color0 = color1` and every index 0. Blocks run left to right, then top to bottom. DXT1 isn't swizzled, unlike the console's uncompressed textures.
- The two end colours are the block's per-channel minimum and maximum; each pixel takes the nearest of the four colours. That's the simplest encoder, and plenty for a logo this size.

## 2. The XPR0 resource

The section's bytes, 10,240 for a 128 × 128 image:

| Offset | Size | Field | Value |
|---|---|---|---|
| `0x000` | 4 | magic | `XPR0` |
| `0x004` | 4 | total size | header size + image bytes (`0x2800`) |
| `0x008` | 4 | header size | `0x800`: the header, padded with zeros |
| `0x00C` | 4 | `Common` | `0x00040001`: a texture, one reference |
| `0x010` | 4 | `Data` | `0`: the image starts at the header's end |
| `0x014` | 4 | `Lock` | `0` |
| `0x018` | 4 | `Format` | `0x07710C29`, below |
| `0x01C` | 4 | `Size` | `0`: the size is in `Format` |
| `0x020` | 4 | end of the resource list | `0xFFFFFFFF` |
| `0x800` | 8,192 | the DXT1 blocks | §1 |

`Format` is the GPU's texture format word: bits 0–1 the DMA context (1), bit 3 the border from the colour (1), bits 4–7 the dimensions (2), bits 8–15 the colour format (`0x0C`, DXT1), bits 16–19 the mipmap levels (1), bits 20–23 and 24–27 the width and height as powers of two (7 and 7), bits 28–31 the depth (0).

## 3. Adding the section

The XBE's image header (`base address` at `0x104`, `size of headers` at `0x108`, `size of image` at `0x10C`, `number of sections` at `0x11C`, `section headers address` at `0x120`; see [xbe.md §1](xbe.md#1-image-header)) and its section headers (56 bytes each: flags, virtual address, virtual size, raw address, raw size, section name address, name reference count, head and tail shared-page count addresses, SHA-1 digest) are changed only like this:

1. The XBE **must not** already have a `$$XTIMAGE` section; the tool refuses one that has.
2. Past the end of the headers, aligned to 4: a copy of the section headers with a new one at the end, then the name `$$XTIMAGE` and its NUL, then (aligned to 2) the new section's head and tail shared-page counts, two 16-bit zeros. The headers **must** still end before the first section starts, in the file and in memory (both `0x1000` bytes in for `cxbe`'s XBEs, which leaves about 2.5 KiB); otherwise the tool refuses the XBE. The old copy of the section headers stays where it was, unused.
3. The new section header: flags `0x8` (an inserted file: not preloaded, so it takes no memory when the app runs); virtual address the first page after the last section's end; virtual and raw size the resource's; raw address the file's end, rounded up to a page (`0x1000`); name reference count 0; and a digest that is the SHA-1 of the size (32-bit) followed by the bytes, as for the other sections.
4. The image header: `size of headers` to the new end, `size of image` to the end of the new section in memory, `number of sections` one more, and `section headers address` to the copy.
5. The resource is appended at that raw address, and the file is padded to a page.

Nothing else changes: every other section's bytes and headers, the certificate and the entry point stay as `cxbe` wrote them. The app never reads the section. On the console, the kernel loads the headers and the preloaded sections as before.

## 4. Changes

| Version | Date | Change |
|---|---|---|
| 1 | 2026-10-28 | First version. |
