# 0016. A settings file, and the install location in it

- **Status:** Accepted (owner decision, 2026-10-27)
- **Date:** 2026-10-27

## Context

M8's SETTINGS gets more than the archive.org login. The owner asked for three settings, in this order: where games install, an Authentication page listing the map's login domains, and editing the map's sources as a list. This ADR covers the first one and where settings are kept.

Until now games always went to `E:\Games`. Larger drives often have extended partitions, F: (partition 6) and G: (partition 7), with far more room than E:. Dashboards such as XBMC4Gamers can list games from any of them.

The app already keeps two files of its own: the map (sources, shared and included) and the credentials file (logins, never shared). Neither is the place for a preference: the map is the user's and may come from elsewhere, and the credentials file is all about secrets.

The owner chose (2026-10-27) a list of `Games` folders on each drive that's there, plus a folder typed on the on-screen keyboard.

## Decision

1. **A third file, `E:\Apps\xcat\settings`**, in the same plain `key = value` style as the others, with its own header and version ([settings.md](../formats/settings.md)). It is parsed in core (`xcat/settings.h`), tested and fuzzed like the other formats. It is all-or-nothing, as the credentials file is; unknown keys are skipped so newer minor versions load. The app writes it one key at a time, keeping every other byte, and replaces it atomically.
2. **`install_dir`**, default `E:\Games`, may be any folder on E:, F: or G: whose names FATX can store, at most 64 bytes after the drive. SETTINGS lists `E:\Games`, `F:\Games` and `G:\Games` for the drives that are there, with their free space, and "Other folder" for typing one. F: and G: are mounted at startup; a drive whose root can't be read isn't offered.
3. **Changing it doesn't move anything.** Games already installed stay where they are. INSTALLED, the marks in GAMES and resuming an unfinished install look in the folder chosen now. Moving games between drives is out of scope.
4. **The install takes the folder's real length and cluster size.** The FATX path limit (ADR 0008) counts from the chosen folder (`xc_install_req.games_dir_len`), and the free-space estimate uses the drive's own cluster size, which is often larger on F: and G:.

## Consequences

- One more file to document and parse, but settings stay out of the map and away from the secrets.
- An unfinished install in the old folder isn't seen after a change; switching back finds it again.
- The largest file FATX takes (`XC_FATX_MAX_FILE_SIZE`) is measured on E:'s 16 KiB clusters. On a drive with larger clusters it is one cluster smaller; an Xbox game file that size is unlikely, and the kernel's error would stop the install, which stays resumable.
- Authentication and the map editor (the next two settings) will add their own pages to SETTINGS; whether they need keys here is decided with them.
