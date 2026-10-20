# Installing from a stream (sequential extraction)

**Status:** Normative for `core/src/seq.c` (`xcat/seq.h`, M5b). It implements [ADR 0009 amendment 1](../adr/0009-install-from-zip-archives.md#amendment-1-2026-10-19-adaptive-spool): installing an image that can only be read front to back, such as a Deflate zip member ([zip.md §9](zip.md#9-reading-a-member)). The spool's files on the console's disk and resuming after a power cut come in a later M5b MR, which adds their sections here.

The words **must**, **must not**, **should** and **may** are used as in RFC 2119. Offsets are absolute in the image.

## 1. Inputs

- **The stream:** the image's bytes in order, each once, from a point the reader asks for. Each pass starts at or before that point; with a zip, at the checkpoint before it ([zip.md §10](zip.md#10-checkpoints)).
- **The sink:** the same as for a random-access install (`xcat/sink.h`): directories, then one file at a time, written front to back.
- **The spool:** numbered chunk files, plus the drive's free space (§4).

## 2. Finding the volume

1. The reader doesn't know the layout until it has seen a volume descriptor ([xdvdfs.md §2](xdvdfs.md#2-volume-descriptor)). The candidates are tried **in stream order**: xiso (partition 0), XGD3 (`0x02080000`), XGD2 (`0x0FD90000`), XGD1 (`0x18300000`). A candidate whose descriptor would end past the image is skipped, as in [xdvdfs.md §1](xdvdfs.md#1-image-layouts).
2. The first candidate whose descriptor is valid is the layout. A random-access reader probes xiso, XGD1, XGD2, XGD3, so the two orders differ only for an image with two valid descriptors, which no real disc has. A descriptor that is invalid but has the leading magic is corrupt here too. No valid candidate: `XC_ERR_NOT_XDVDFS`.
3. The 64 KiB of the partition before its descriptor are kept in memory until the decision, then treated like any other unidentified bytes (§3.2). Nothing before the partition is game data.

## 3. Passes

### 3.1 Directory tables

- The root table's extent comes from the descriptor. Every other table's comes from its parent's entry. The reader captures the bytes it needs of each table it knows about ([xdvdfs.md §3](xdvdfs.md#3-directory-tables), rule 1) as they pass.
- When a table is complete, the reader lists the image again with `xc_xdvdfs_list_partial()` ([xdvdfs.md §9](xdvdfs.md#9-partial-listings)), using only the tables captured so far, with every check and limit of a normal listing. Tables that the new listing names and that have already passed are taken from the spool where it has their bytes, and the listing is repeated until nothing more can be filled in.
- **All known:** no table is pending any more. From then on, every byte is identified.
- After each listing, the caller sees it (for the FATX checks, ADR 0008), then the new directories are created, parents first.

### 3.2 Bytes as they pass

For each byte, in this order:

1. inside a file whose directory is known, that isn't written yet, and that starts at or after where the stream was when it became known (a **direct** file): written to it;
2. inside a table the reader still needs: captured;
3. otherwise, while not all is known, and from the partition start on: **unidentified**, so it is spooled (§4), unless it's already in the spool, or belongs to a file already written (a later pass);
4. otherwise: discarded.

A file that becomes known after its first byte has passed is **late**. Empty files are written as soon as they're known. Two non-empty files that share bytes can't both be written from one stream, so the install is refused (`XC_ERR_UNSUPPORTED`).

### 3.3 End of a stream

A pass stops reading as soon as all is known and every direct file is written. Then:

1. Every late file whose bytes are all in the spool is copied out of it, in offset order. Once all is known, a chunk is deleted as soon as no remaining late file needs it.
2. If anything is still missing, another pass is needed (§3.4). Otherwise the install is done, and any chunk left is deleted.

### 3.4 Further passes

- The next pass starts at the earliest missing byte: of a table still needed, or of a late file the spool doesn't fully have.
- In that pass, every file still to be written that starts at or after that point is direct, including late files the spool lost.
- A pass that starts at a still-needed table always captures it, so each pass learns at least one more table or writes at least one more file.
- The number of passes is limited (default 16, `XC_ERR_LIMIT` beyond).

## 4. The spool

1. Chunk *k* holds the unidentified bytes of image interval `[k × C, (k + 1) × C)`, with C = 64 MiB by default (the checkpoint spacing), appended in stream order. The reader keeps an index from image offsets to chunk positions.
2. **Space:** the spool and the game share the drive, which always keeps the **reserve** free. Before writing anything (a file's full size, at its start, or spool bytes), the reader checks the free space it expects, rounded to the cluster size. If there isn't enough, it asks the drive for the real figure and deletes chunks until there is:
   - while streaming, the **oldest** chunk first (the lowest interval), because late files sit just before the tables that list them (ADR 0009 amendment 1);
   - while copying a late file out, the chunk **furthest** from it first, never one that holds the file itself, so the next pass starts as late as possible.
3. When even an empty spool can't make room: spool bytes are simply not kept (a later pass reads them again), and a file can't be written at all (`XC_ERR_NO_SPACE`).

## 5. Result

The reader reports the volume, the number of passes, the files written as they passed and the files copied from the spool, the bytes streamed and those re-read in later passes, and the spool's peak, total and dropped bytes. When every pass is done, every file in the final listing is on disk and has been reported to the caller with its CRC-32, and the spool is empty.

## 6. Security

The image is untrusted. Every table goes through the normal listing checks (xdvdfs.md §3–§6). Memory is bounded by the tables of the listing limits and the spool's index. `fuzz_seq` runs the reader on fuzzed images with drive sizes from roomy to tight, and checks every file's CRC-32 against the image (CONTRIBUTING.md#fuzzing).
