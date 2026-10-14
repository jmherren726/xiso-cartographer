# 0008. Refuse games whose files FATX can't store; choose the install folder name ourselves

- **Status:** Accepted
- **Date:** 2026-10-13

## Context

A disc's filesystem (XDVDFS) allows names of up to 255 bytes, containing almost any byte. The console's hard disk (FATX) allows names of up to 42 bytes, rejects 85 byte values, limits paths to 250 bytes and directories to 4,096 entries. These limits were measured on the console's own kernel by `app/fatx-probe` ([fatx-names.md](../formats/fatx-names.md), [testing.md](../testing.md#fatx-probe-m2)). So some images can't be installed as they are.

There were two options:

- **Rename** offending files: shorten them and replace bad bytes. The install "succeeds", but a game opens its files by name. A renamed file is a missing file, and the game breaks, often only once play reaches that file.
- **Refuse** the install, and say exactly which paths are the problem.

The install folder (`E:\Games\<folder>`) is different: no game refers to it by name, so we can choose it.

## Decision

1. Before anything is written, every entry of the image is checked against the FATX rules, using the real install folder's path length (`xc_fatx_check_listing`). Any problem **refuses the install** with `XC_ERR_NOT_INSTALLABLE` and a list of every problem. The app never renames, truncates or skips a game's files.
2. The install folder name is generated from the title by `xc_fatx_title_name()` and always made valid. If the folder is taken, a numbered variant is used (`xc_fatx_numbered_name()`).
3. The same check is available on a PC (`xcat-cli check`), so users can find affected images before copying anything.

## Consequences

- An installed game always has exactly the files the disc has. There are no silently broken installs.
- A few images can't be installed by this app. The message points to the cause, and users can turn to PC tools that repackage games (for example, a Games on Demand conversion) or to a shorter install folder name. A shorter title gives deep paths a little more room.
- Whether real retail games hit these limits isn't known yet. That will show once real images are installed (M5/M9). If it's common, a later ADR can revisit this, for example by offering a shorter folder name automatically.
- The rules come from measurement, not documentation. If a kernel version behaves differently, the probe is rerun and the table updated (fatx-names.md).
