# 0006. No links, no bypasses, good-guest networking

- **Status:** Accepted; amended by [ADR 0007](0007-scoped-user-credentials.md) (logging in with the user's own, scoped credentials is allowed)
- **Date:** 2026-10-13

## Context

xiso-cartographer downloads disc images. That's legitimate for backups the user is entitled to use, but the same tool could be pointed at content the user has no right to, or at sites that deliberately block automated downloads. The project's legal footing, and whether it can stay hosted and shared, depends on what the project itself ships and does, not on what a user might do with it (HANDOFF §1.3, §6.3).

## Decision

**What the project never contains** (code, docs, issues, fixtures, CI, releases):

- game images, ROMs, BIOS/MCPX dumps or Microsoft XDK material;
- download links, item identifiers or source lists for copyrighted games, including in examples. Examples use placeholders (`<item>`, `example.com`, private LAN addresses).

**What the software never does:**

- get past captchas, Cloudflare or other bot or access protection, scrape protected download flows, or fake a browser to get around them;
- ignore a source's access controls: `401`/`403` are reported to the user and never retried with workarounds;
- host, mirror, seed or relay images for other users.

**How it chooses sources:** only from the user's map ([map.md](../formats/map.md)). The app has no built-in source list and never suggests where to find a game. `@archiveorg` only expands an identifier the user supplied.

**How it behaves on the network** ("good guest"):

- at most **2** concurrent connections to the same host (archive.org included); one per install by default;
- a clear `User-Agent`: `xiso-cartographer/<version> (+<project URL>)`;
- exponential backoff with jitter on errors and on `429`/`503`, honouring `Retry-After`; a hard cap on retries per request;
- no polling or background traffic unless the user starts an action.

## Consequences

- Reviewers reject any MR that adds links, identifiers or content from the first list, or code from the second. The MR template's checklist already has a line for this.
- Sources behind bot protection (e.g. sites that need a browser session) stay out of scope permanently. Users can download from those sites manually and serve the files from their own LAN server.
- Downloads can be slower than an aggressive client would get. We accept that.
- Feature requests that conflict with this ADR are closed with a link to it.
