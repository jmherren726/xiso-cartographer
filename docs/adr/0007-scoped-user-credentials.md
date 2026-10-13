# 0007. Scoped user credentials for sources that need a login

- **Status:** Accepted (design only; implemented with the HTTP client in M3/M5 and the archive.org source in M6)
- **Date:** 2026-10-13
- **Amends:** [ADR 0006](0006-no-links-no-bypass.md)

## Context

Some sources a user is entitled to use need a login:

- archive.org items that are only available to logged-in accounts. A browser download carries the session cookies (`logged-in-user`, `logged-in-sig`).
- LAN or private servers that use HTTP Basic auth or a bearer token.

[ADR 0006](0006-no-links-no-bypass.md) forbids ignoring a source's access controls. It says nothing about the user authenticating *as themselves*, with their own account. That needs a decision, because doing it carelessly creates real risks:

- **Leaks.** Maps are made to be shared, pulled in with `@include` and fetched over the LAN ([map.md](../formats/map.md)). On the console they sit on a FATX drive that's readable over FTP, often with the default `xbox`/`xbox` login. A password there is exposed to anyone on the network.
- **Exfiltration.** A token attached to "all requests" would be sent to whatever host a map entry or a redirect points at.
- **Bypass by another name.** A generic "post to any login form and capture the token" feature means scraping login flows: hidden fields, anti-forgery tokens, JavaScript, sometimes captchas. That is exactly what ADR 0006 rules out.

## Decision

1. **Credentials live in their own file, never in a map.** The format is [credentials.md](../formats/credentials.md). The file is only ever read locally: never through `@include`, never from a URL. Maps keep rejecting `user:pass@` URLs.
2. **The console stores tokens, not passwords.** The file holds what goes into a request: a cookie value or a header. Getting a token is done off the console, either by the user, or by a small PC helper (`tools/xcat-login`, built in M6) that asks for the password, calls the service's login API and writes only the resulting token. On-box login can come later, once the UI has text entry (M8). Even then, only the token is stored.
3. **Every credential is scoped.** It's sent only when all of these hold:
   - the request goes over **HTTPS**, or over plain HTTP to a private IPv4 address when the section explicitly opts in;
   - the request's host matches the section's `hosts` list;
   - and this is re-checked on every redirect hop.

   A redirect to a host outside the scope is followed **without** the credential.
4. **The user chooses where the token goes:** a cookie, or named request headers. Query-string tokens aren't supported, because they end up in server and client logs. Headers that would break or smuggle requests (`Host`, `Content-Length`, `Transfer-Encoding`, `Connection`, `Range`) can't be set.
5. **Logins are automated only through documented APIs.** `xcat-login` gets one provider per service, and only for login APIs the service documents for programmatic clients. The first is archive.org, using the login the official `internetarchive` tooling uses. The exact endpoint will be confirmed when it's built. There's no generic form-login automation, ever.
6. **ADR 0006 still applies in full.** `401`/`403` responses are reported, e.g. "login missing or expired for `[archive]`", and never retried with workarounds. Using your own account doesn't make every download permitted: restricted collections (such as lending) have their own terms, and the docs say so.
7. **Secrets never reach logs.** Logs name the credentials section that was used, never its values.

## Consequences

- Users can install from their own logged-in sources without ever putting a password on the console.
- A shared or included map can't carry or redirect a user's credentials anywhere.
- The HTTP client (M3) must apply credentials per request hop, so the redirect logic needs them in mind from the start. ARCHITECTURE.md records that.
- A leaked credentials file still exposes a session. The docs tell users to change the console's FTP password, and to revoke a token by logging out or changing their password if the console is lost or shared.
- Each new login provider in `xcat-login` needs review against this ADR.
