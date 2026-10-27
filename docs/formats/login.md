# archive.org login on the console

**Status:** Normative. §1–§4 are implemented by `xcat/login.h` (M8) and §2 by `xc_http_post` ([http.md §10](http.md#10-post-for-logins)). §5 is implemented by `xc_cred_set_section` (`xcat/credentials.h`) and `xc_nx_replace_file` (`xcat/nxdk.h`), and §6 by the console's SETTINGS section. The reasons are in [ADR 0010](../adr/0010-on-box-login-required-for-beta.md) and [ADR 0007](../adr/0007-scoped-user-credentials.md).

The user logs in to archive.org on the Xbox with their email address and password. The password is sent once and never stored. Only the session that archive.org returns is written, as the `[archive]` section of the [credentials file](credentials.md).

The words **must**, **must not**, **should** and **may** are used as in RFC 2119.

## 1. The request

The login uses the same API as archive.org's official `internetarchive` tooling (its `ia configure`, `get_auth_config`). ADR 0007 decision 5 allows only this kind of documented login API. Login forms are never scraped.

```
POST https://archive.org/services/xauthn/?op=login
Content-Type: application/x-www-form-urlencoded

email=<email>&password=<password>
```

- The email and password are each **1–254** and **1–128** bytes. They **must not** contain control characters (below `0x20`, or `0x7F`). The on-screen keyboard types printable ASCII only.
- Both are form-encoded: every byte other than `A–Z a–z 0–9 - . _ *` becomes `%XX` (upper-case hex), so a space becomes `%20`.
- archive.org logs in with the account's **email address**. ADR 0010 says "username"; for archive.org this means the email.

## 2. Sending it

The request goes out through `xc_http_post` ([http.md §10](http.md#10-post-for-logins)):

- over `https` only, with certificate checks;
- on a fresh connection that is closed afterwards;
- **without** the credentials file's cookies or headers;
- once, with no retries and no redirects. A redirect, or any status other than `200`, is a failed login.

The answer is at most **16 KiB**.

## 3. The answer

A JSON object ([archiveorg.md §6](archiveorg.md#6-json-subset) applies), with these members. All others are ignored.

| Member | Meaning |
|---|---|
| `success` | `true` for a login. Anything else, or no `success` at all, means no login. |
| `values.cookies.logged-in-user`, `values.cookies.logged-in-sig` | The session. A login **must** have both. Each is a string of **1–1024** cookie-octets ([RFC 6265 §4.1.1](https://www.rfc-editor.org/rfc/rfc6265#section-4.1.1): printable ASCII except space, `"`, `,`, `;` and `\`). |
| `values.reason`, else `error` | Why there was no login, when there wasn't. `account_not_found` and `account_bad_password` get their own messages. Any other reason is shown and logged as given: printable ASCII only, cut at the first other character and at **64** bytes. |

- An answer that isn't JSON is rejected (`XC_ERR_CORRUPT`, "not a login answer"). So is a `success: true` without both cookies, or a cookie that isn't cookie-octets.
- The answer may hold other secrets, such as `values.s3`. They are never read or kept.

## 4. The section written

```
[archive]
# Written by the on-box login. Log out in SETTINGS.
hosts = archive.org, *.archive.org
cookie = logged-in-user=<logged-in-user>; logged-in-sig=<logged-in-sig>
```

The values go in exactly as received. That is the form the `internetarchive` tooling sends them in, and the form [credentials.md §1](credentials.md#1-example) shows.

## 5. Writing the credentials file

A login writes the section of §4 under the name `archive`. Logging out removes it. Either way:

1. The file is read first. If it exists and doesn't parse ([credentials.md §7](credentials.md#7-errors)), nothing is written, and the user is told the line and the problem: it isn't the login's file to fix.
2. The section `[archive]` (its name compared without regard to case) runs from its `[…]` line through its last key line. Blank lines and comments after the last key stay where they are, since they may belong to the next section.
3. That range is replaced by the new section, or removed on log out. If there's no such section, a login adds it at the end of the file: after a line ending if the file has none, then a blank line. A login with no file at all starts a new file: `# xcat-credentials v1`, a blank line, then the section.
4. Every other byte of the file stays as it was: other sections, comments, the BOM and line endings.
5. The result **must** parse too. If it doesn't, nothing is written. For example, another section might already cover `archive.org`, so the patterns would overlap (credentials.md §5).
6. The file is replaced atomically. The new text goes to `credentials.tmp`, which is flushed and then renamed over `credentials`. A power cut leaves either the old file or the new one. If writing or renaming fails, `credentials.tmp` is overwritten with zeros and deleted.

## 6. Secrets

- The password exists only in the keyboard's buffer and in the request body. The keyboard shows dots, and the text never reaches the drawing code ([ui-style.md](../ui-style.md#widgets)). The password is wiped as soon as the body is made, or when the user cancels. The body is wiped once it is sent: `xc_http_post` wipes its own buffer where the request was. TLS encrypts it in place in BearSSL's record buffer.
- The session is wiped from memory once the file is written. That includes the parsed answer, the section text and the file's old and new text.
- Logs and messages name the section (`[archive]`), never a value. Not the email, the password or a cookie.
- Logging out on the console removes the section. The session itself ends only when the user logs out on archive.org or changes the password there. The console says so.

## 7. Changes

| Version | Date | Change |
|---|---|---|
| 1 | 2026-10-27 | First version (M8). |
