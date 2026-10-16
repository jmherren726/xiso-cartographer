# HTTP client profile, version 1

**Status:** Normative. §2, §4 and §5 are implemented by `xcat/url.h` and `xcat/http.h`; §3 and §6–§9 by the HTTP client (`xcat/http_client.h`). The reasons behind the design are in [ADR 0004](../adr/0004-streaming-install.md) (Range is required), [ADR 0006](../adr/0006-no-links-no-bypass.md) (good-guest networking) and [ADR 0007](../adr/0007-scoped-user-credentials.md) (credentials per hop).

xiso-cartographer speaks a small, strict subset of HTTP/1.1 ([RFC 9110](https://www.rfc-editor.org/rfc/rfc9110), [RFC 9112](https://www.rfc-editor.org/rfc/rfc9112)): `GET` with `Range`, nothing else. Every byte the server sends is untrusted (HANDOFF §7.3). Where this profile is stricter than the RFCs, it rejects rather than guesses, because a misread response could write wrong bytes into a game.

The words **must**, **must not**, **should** and **may** are used as in RFC 2119.

## 1. Limits

| Limit | Value |
|---|---|
| URL length | 2048 bytes (before percent-encoding) |
| Host name | 253 bytes |
| Request target after percent-encoding | 4096 bytes |
| Response head (status line + headers + blank line) | 16 KiB |
| Header lines per response | 100 |
| Chunk-size line, each trailer line | 1 KiB |
| Trailer section | 16 KiB |
| Redirects per request | 5 |

Exceeding any limit fails the request.

## 2. URLs

### 2.1 Accepted URLs

```
url       = scheme "://" authority [ path-query ]
scheme    = "http" / "https"                 ; case-insensitive
authority = host [ ":" port ]
host      = reg-name / IPv4address
port      = 1*5DIGIT                          ; 1-65535
```

- `reg-name` is 1–253 bytes of `A–Z a–z 0–9 - .`, with no empty labels (no leading, trailing or doubled dots). It is lowercased.
- An `IPv4address` is four decimal numbers 0–255 without leading zeros. IPv6 literals (`[…]`) are not supported in v1.
- A host whose last label starts with a digit **must** be a valid `IPv4address`. Otherwise a resolver could read `1.2.3` or `0x7f.1` as an address while a credentials pattern sees a name ([credentials.md §5](credentials.md#5-host-patterns)).
- The default port is 80 for `http` and 443 for `https`.
- A URL **must not** contain user credentials (an `@` in the authority), a fragment (`#`) or control characters (bytes below `0x20`, and `0x7F`). Logins come from the [credentials file](credentials.md), never from a URL.
- `path-query` starts at the first `/` or `?` after the authority. An empty path becomes `/`.

### 2.2 Request target

The path and query are sent as written, except that these bytes are percent-encoded as `%XX` (uppercase hex): space, `"`, `<`, `>`, `\`, `^`, `` ` ``, `{`, `|`, `}`, every byte ≥ `0x80`, and a `%` that isn't followed by two hex digits. Existing `%XX` escapes are kept as they are ([map.md §4.2](map.md#42-url)). So `http://192.168.1.20:8080/xbox/Halo (Europe).iso` requests `/xbox/Halo%20(Europe).iso`.

### 2.3 Redirect targets

A `Location` value is resolved against the URL of the request that returned it ([RFC 3986 §5.2](https://www.rfc-editor.org/rfc/rfc3986#section-5.2)):

| `Location` form | Example | Result |
|---|---|---|
| Absolute URL | `https://dn1.example.org/a.iso` | that URL, checked as in §2.1 |
| Network-path | `//dn1.example.org/a.iso` | the current scheme plus that authority and path |
| Absolute path | `/items/a.iso` | the current scheme and authority with that path |
| Relative path | `a.iso`, `../b/a.iso` | merged with the current path; `.` and `..` segments removed |

A `Location` that is empty, contains control characters or doesn't parse is a protocol error. A fragment in `Location` is dropped.

## 3. Requests

Each request is exactly:

```
GET <target> HTTP/1.1
Host: <host>[:<port>]              (port only when it isn't the scheme's default)
User-Agent: xiso-cartographer/<version> (+https://github.com/jmherren726/xiso-cartographer)
Accept-Encoding: identity
Range: bytes=<first>-<last>
<credential headers, see §8>
```

Lines end in CRLF and the head ends with an empty line. There is no body. `Connection: keep-alive` is implied by HTTP/1.1.

## 4. Response head

### 4.1 Framing

- The head ends at the first empty line. Lines end in CRLF; a bare LF is also accepted ([RFC 9112 §2.2](https://www.rfc-editor.org/rfc/rfc9112#section-2.2)).
- The status line is `HTTP/1.<digit> <3-digit status> [reason]`. The status is 100–599. Any `1xx` response is a protocol error: the client never sends `Expect`, so a server has no reason to send one.

### 4.2 Header lines

- A header line is `name ":" OWS value OWS`. `name` is a non-empty token (`A–Z a–z 0–9` and ``!#$%&'*+-.^_`|~``). Whitespace between the name and the colon, and obsolete line folding (a line starting with a space or tab), are protocol errors ([RFC 9112 §5](https://www.rfc-editor.org/rfc/rfc9112#section-5)).
- Values **must not** contain control characters other than horizontal tab. Bytes ≥ `0x80` are allowed (and ignored).
- Header names are matched case-insensitively. Unknown headers are ignored.

### 4.3 Headers the client uses

| Header | Rule |
|---|---|
| `Content-Length` | 1–19 decimal digits. Repeated headers, or a comma-separated list, must all carry the same value. |
| `Transfer-Encoding` | Only `chunked` (case-insensitive) is supported; any other coding is a protocol error. Together with `Content-Length` it is a protocol error (a request-smuggling pattern, [RFC 9112 §6.3](https://www.rfc-editor.org/rfc/rfc9112#section-6.3)). |
| `Content-Encoding` | Absent or `identity`. The client asks for `identity`, so anything else is a protocol error. |
| `Content-Range` | `bytes <first>-<last>/<total>` or `bytes <first>-<last>/*`, with `first ≤ last < total`; or `bytes */<total>` (only meaningful on `416`). Numbers are 1–19 decimal digits. At most one. |
| `Location` | Used on redirects (§6). At most one. |
| `Retry-After` | Delay in seconds (1–10 digits). The HTTP-date form is ignored. |
| `ETag`, `Last-Modified` | Stored as opaque strings (at most 127 bytes each, longer values are ignored) to notice a file that changed between requests (§7.3). |
| `Connection` | A `close` token means the connection isn't reused. |

## 5. Response body

- **`Transfer-Encoding: chunked`:** `chunk-size` is 1–16 hex digits, at most 2⁶³−1. Chunk extensions (`;…`) are ignored. Each chunk's data must be followed by CRLF (or LF). The last chunk is `0`, followed by optional trailer lines that are read and ignored, then an empty line ([RFC 9112 §7.1](https://www.rfc-editor.org/rfc/rfc9112#section-7.1)).
- **`Content-Length`:** exactly that many bytes.
- **Neither:** the body runs until the server closes the connection. This is accepted only when the length is known from `Content-Range` (§6), so a connection that drops early is always noticed.

## 6. Status codes

| Status | Meaning to the client |
|---|---|
| `206` | Success. `Content-Range` must be present with a known total (`/*` is rejected: the client must know the file's size), with `first` equal to the requested first byte and `last` not beyond the requested last byte. `total` must equal the size learned from earlier responses. A shorter range than requested is accepted, and the rest is requested next. |
| `200` (to a `Range` request) | The server ignores `Range`: "this server doesn't support resumable downloads" ([ADR 0004](../adr/0004-streaming-install.md)). Not retried. |
| `301`, `302`, `303`, `307`, `308` | Redirect to `Location` (§2.3), at most 5 per request. Each hop is a new request, and credentials are decided again for it (§8). The final URL is remembered for later requests to the same file. |
| `401`, `403` | Access denied. Reported, never retried ([ADR 0006](../adr/0006-no-links-no-bypass.md)). |
| `404`, `410` | Not found. |
| `416` | Range not satisfiable: the file is shorter than expected. |
| `408`, `429`, `500`, `502`, `503`, `504` | Temporary: retried (§7). |
| Anything else | Failure, reported with the status code. |

## 7. Retries and resume

### 7.1 Timeouts

Connecting (including DNS) and each wait for data have a timeout (default 15 s). A timeout counts as a network error.

### 7.2 Retries

- Network errors (DNS, connect, timeout, a connection closed before the body is complete) and the temporary statuses of §6 are retried. Protocol errors, `200` instead of `206`, denied and not-found are not. Neither are TLS failures while connecting (a rejected certificate, no acceptable time, no randomness, a failed handshake; [ADR 0012](../adr/0012-tls-trust-time-and-entropy.md)): a retry can't change them.
- The delay before retry *n* (1, 2, …) is `min(base × 2^(n−1), max)` with base 1 s and max 30 s, then a random value between half of that and all of it ("jitter"). A `Retry-After` from the server replaces it, capped at 120 s.
- A request gives up after a fixed number of consecutive failed attempts (default 8; the console's network test uses more). The count starts again whenever bytes arrive, so a long download survives any number of short outages, such as a pulled cable.

### 7.3 Resume

After a failure in the middle of a body, the next attempt asks for the remaining bytes only (`Range: bytes=<next>-<last>`). Bytes already passed on are never requested again.

Each response's `Content-Range` total, `ETag` and `Last-Modified` are compared with the first response's. If the server reports a different size, or either validator differs when both responses carry it, the file changed during the download, and the request fails: mixing two versions of a file would corrupt the install.

## 8. Credentials

Before every request hop, including each redirect target, the client asks the credentials module which headers to add ([credentials.md §6](credentials.md#6-when-credentials-are-sent)). The decision depends only on that hop's scheme, host and port. The client never stores `Set-Cookie`. A `401`/`403` on a hop that carried credentials is reported naming the credentials section, never its values.

## 9. Connections

- `https` connections run TLS 1.2 over the plain connection ([`xcat/tls.h`](../../core/include/xcat/tls.h)); the URL's host is the SNI name and the name the certificate must carry. Certificate checks can't be turned off.
- One connection at a time. It is reused for the next request to the same scheme, host and port when the previous response was read completely, was a `2xx`, and didn't say `Connection: close`. Otherwise it is closed.
- Any bytes after the end of a body (the client never pipelines) mean the response can't be trusted to have ended where it says, so the connection is closed.

## 10. Changes

| Version | Date | Change |
|---|---|---|
| 1 | 2026-10-14 | First version. |
| 1 | 2026-10-16 | Clarified for M5: TLS failures aren't retried (§7.2); how `https` connects (§9). No change for plain HTTP. |
