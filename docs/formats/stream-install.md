# Installing from a stream (sequential extraction)

**Status:** Normative for `core/src/seq.c` (`xcat/seq.h`, M5b), which `xc_install` uses for Deflate zip members (`xcat-cli install <zip>`). There, the spool chunks are files named `.xcat-spool-<k>` in the install folder, kept in the drive's free space minus a 256 MiB reserve, and the stream restarts at the zip checkpoint before each later pass ([zip.md §10](zip.md#10-checkpoints)). It implements [ADR 0009 amendment 1](../adr/0009-install-from-zip-archives.md#amendment-1-2026-10-19-adaptive-spool): installing an image that can only be read front to back, such as a Deflate zip member ([zip.md §9](zip.md#9-reading-a-member)). After a power cut, an install continues from the last checkpoint where it saved its state (§7).

The words **must**, **must not**, **should** and **may** are used as in RFC 2119. Offsets are absolute in the image.

## 1. Inputs

- **The stream:** the image's bytes in order, each once, from a point the reader asks for. Each pass starts at or before that point; with a zip, at the checkpoint before it ([zip.md §10](zip.md#10-checkpoints)).
- **The sink:** the same as for a random-access install (`xcat/sink.h`): directories, then one file at a time, written front to back.
- **The spool:** numbered chunk files, plus the drive's free space (§4). To save states (§7), the spool can also report a chunk file's size and make its appends durable.

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

A file that becomes known after its first byte has passed is **late**. A file an earlier attempt finished (the caller says which, [journal.md §6.8](journal.md)) counts as written. Empty files are written as soon as they're known. Two non-empty files that share bytes can't both be written from one stream, so the install is refused (`XC_ERR_UNSUPPORTED`).

### 3.3 End of a stream

A pass stops reading as soon as all is known and every direct file is written, except that a caller may ask for the whole of pass 1 (a zip member's CRC-32 then covers every byte, [zip.md §9](zip.md#9-reading-a-member)). Then:

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

The image is untrusted. Every table goes through the normal listing checks (xdvdfs.md §3–§6). Memory is bounded by the tables of the listing limits and the spool's index. A saved state (§7) is read back from the disk, so it's checked like any input: its CRC-32 and every field's bounds (§7.2), and the descriptor and tables again as they're used. `fuzz_seq` runs the reader on fuzzed images with drive sizes from roomy to tight, cuts the power at some point of some pass and resumes, and checks every file's CRC-32 against the image. It also resumes from states with one byte forged (and the CRC-32 fixed), to check that a forged state can't break memory safety (CONTRIBUTING.md#fuzzing).

## 7. Resuming after a power cut

### 7.1 Saving a state

1. At each checkpoint of the stream (for a zip, each zip checkpoint, [zip.md §10](zip.md#10-checkpoints)), the reader saves its **state**: what it needs to go on from that offset as if it had never stopped. First the file being written is flushed to disk, then the spool's appends are made durable, so the state only describes data on disk.
2. No state is saved:
   - while the layout isn't known, inside the bytes kept for a candidate's decision (§2.3);
   - when the stream isn't at that offset yet (a later pass that restarted at an earlier checkpoint);
   - when the sink or the spool can't make their data durable;
   - when the state would be larger than 64 MiB.
3. `xc_install` keeps three things in the install folder for this:
   - **`.xcat-zip-checkpoints`:** every zip checkpoint, appended in order, durably, before any state names its offset;
   - **`.xcat-state-0` and `.xcat-state-1`:** state *n* is written whole to `.xcat-state-<n mod 2>`, durably, and then a `state` line names it ([journal.md §5](journal.md#5-records)). The file the last `state` line names is never written to, so a power cut at any moment leaves it whole;
   - **`drop` lines:** once a state exists, a `drop <chunk>` line comes before a chunk file is deleted.

   They're deleted once the install has finished.
4. **Space:** these files take space from the spool's share, not the reserve: the free space the reader sees doesn't count the checkpoints still to come, nor 64 KiB more than the last state for each state file.

### 7.2 State format

Numbers are little-endian. A state is:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | Magic `XCS1` |
| 4 | 4 | Pass: 1 up to the pass limit |
| 8 | 8 | Image size |
| 16 | 8 | Chunk size C (§4.1) |
| 24 | 8 | **Offset:** the next byte the stream delivers; at most the image size |
| 32 | 4 | Flags: bit 0 set once the volume is known; other bits 0 |
| 36 | 4 | Candidate: the layout (index into §2.1's list) once known; before that, the next candidate, whose partition starts at or after the offset (4 = none left) |

Then, in this order:

1. **Descriptor:** once the volume is known, its 2048-byte sector as read. It is checked again on resume and must give the candidate's partition.
2. **Tables:** a 4-byte count, then, in increasing offset, each table the reader wants: 8-byte offset, 4-byte length (the bytes it needs, 1 to 256 KiB + 2 KiB, inside the image), 4-byte count of bytes captured. For an incomplete table, a bitmap of ⌈length / 8⌉ bytes follows (bit *k* mod 8 of byte *k* / 8 is set if byte *k* was captured, and the count must match). Then `length` bytes of the table, zero where not captured.
3. **Spool index:** a 4-byte count, then each span: 8-byte image offset, 8-byte length (more than 0), 8-byte position in its chunk file. Spans are in increasing image offset, don't overlap, and each lies inside one chunk interval: its chunk is its offset / C.
4. **Open file:** a 4-byte path length (0 if no file was being written, else less than 256), the listing path, and the 4-byte CRC-32 of the file's bytes so far (0 if none). The file has the bytes before the offset: offset − its offset.
5. The CRC-32 of every byte before it.

Before the volume is known, there are no tables, spans or open file. A state that breaks any of these rules is **damaged**.

### 7.3 Resuming

1. The installer reads the journal ([journal.md §6.9](journal.md#6-reading-a-journal)): the last `state` line, and the `drop` lines after it. It reads the checkpoints file, keeping the checkpoints that are valid for the member, up to the first that isn't (a torn append); if anything followed, the file is rewritten with only those. It reads the state file the line names.
2. The state is used only if its size and CRC-32 match the `state` line, its header matches the image size and chunk size, and a checkpoint exists at its offset. Otherwise the install starts the stream again from its start, keeping the finished files ([journal.md §6.8](journal.md#6-reading-a-journal)).
3. The chunk files that `drop` lines after the state name are deleted, since they may since have been made again with other bytes.
4. The reader restores the state. It asks each chunk file's size: a span that ends past it is forgotten (its bytes are read again if needed), a file no span needs is deleted, and a kept chunk's usage is its file's size, so bytes appended after the state still count against the space without being used.
5. It lists the image from the restored tables (§3.1), with the FATX checks and the directories, and classifies the files at the state's offset (§3.2): a file the journal has as done counts as written.
6. **The open file:** unless the journal has it as done, the sink reopens it, cut back to the bytes it had, and it's continued with the saved CRC-32. If the sink can't (the file is gone or shorter), it's a late file.
7. The pass goes on from the state's offset, with the zip read restarted at the checkpoint there; the member's CRC-32 goes on from the checkpoint's. A damaged state that only the restore can tell (§7.2) is set aside, and the install starts again at the image's first byte.
8. **Without a state**, every chunk file found when the install starts is from an earlier attempt and is deleted first.
9. After an error that isn't a power cut (cancelled, the network), the open file is kept if a saved state continues it, and removed otherwise.
