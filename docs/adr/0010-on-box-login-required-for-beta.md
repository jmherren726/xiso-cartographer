# 0010. Logging in on the Xbox is required for the beta

- **Status:** Accepted (owner decision, 2026-10-15)
- **Date:** 2026-10-15
- **Amends:** [ADR 0007](0007-scoped-user-credentials.md) (decision 2, and "the app never writes the credentials file")

## Context

[ADR 0007](0007-scoped-user-credentials.md) keeps passwords off the console: the credentials file holds only tokens, and a token is obtained off the console by the user or by a PC helper (`tools/xcat-login`, M6). It left on-box login as something that "can come later, once the UI has text entry (M8)".

The project's premise is Xbox-only use with no PC needed ([HANDOFF.md §1](../HANDOFF.md)). A beta that needs a PC to log in to archive.org, and again every time the session expires, breaks that premise for every item that needs a login. The owner decided on 2026-10-15 that on-box login is a beta requirement, not an option.

What ADR 0007 protects against is unchanged: the console's drive is readable over FTP, often with the default `xbox`/`xbox` login, so a password stored there is exposed to anyone on the network.

## Decision

1. **The beta logs in on the Xbox, with no PC helper.** M8 builds the login screen (username and password on an on-screen keyboard, from settings and whenever a login is needed). M9 verifies it on hardware. Both exit tests say so ([HANDOFF.md §4.1](../HANDOFF.md#41-roadmap)).
2. **The password is used once and never stored.** It is held in memory only for the login request, sent only over HTTPS with certificate checks (M5), and wiped from memory afterwards. It is never written to the credentials file, a log, a cache or any other file.
3. **The app may write the credentials file, only for a login.** A successful on-box login writes or replaces that service's one section (e.g. `[archive]`) with the token it received, through a temporary file and a rename so a power cut can't leave a half-written file. Every other section is kept byte for byte. Logging out on the console removes the section. This replaces credentials.md §8's "the app never writes this file".
4. **Expired sessions ask; they don't refresh silently.** A `401`/`403` from a host that had a token attached still stops the request and is reported, as in ADR 0006 and 0007 decision 6. The message offers "log in again" and, after a new login, the user can resume the install. Nothing is retried in the background, and no stored secret makes a silent refresh possible.
5. **Same providers, same rules.** On-box login uses only the documented login API that ADR 0007 decision 5 allows for `xcat-login` (for archive.org, the one the official `internetarchive` tooling uses, confirmed when it's built). No form scraping, ever.
6. **`tools/xcat-login` stays optional.** It may still ship in M6 as a convenience and to test the archive.org source before the UI exists, but nothing in the beta depends on it. *(2026-10-20: not built in M6, owner decision; see [ADR 0013](0013-archiveorg-source.md) decision 6.)*

## Consequences

- The beta works for login-required items with only the Xbox, which is the project's premise.
- The login screen needs text entry with an on-screen keyboard; ADR question 4 (UI approach, before M8) must account for it.
- The app now writes `E:\Apps\xcat\credentials`. Writes must be atomic and must preserve the other sections, and that path gets its own tests.
- The password still crosses the console's memory, so it must be wiped after use and must never reach a log; code review checks for that.
- A user still has to log in again by hand when a session expires. That's the cost of not storing the password, and it's accepted.
