# 0012. TLS trust: CA bundle, time source and randomness

- **Status:** Accepted (owner decisions on the time fallback and the hash scope, 2026-10-15)
- **Date:** 2026-10-15
- **Amends:** [ADR 0005](0005-bearssl-for-tls.md) ("If no trusted time is available, HTTPS fails")

## Context

[ADR 0005](0005-bearssl-for-tls.md) chose BearSSL, TLS 1.2 only, validation always on, a CA bundle from the Mozilla root store that a file on the HDD can replace, and SNTP for the clock. Starting M5 needs the details it left open:

- **Where the code lives** and how BearSSL is pinned. Upstream is `https://www.bearssl.org/git/BearSSL`, not GitHub. Its last tag (v0.6, 2018) is older than the fixes on its master branch.
- **Which CA bundle**, how it's pinned and updated, and what the file on the HDD does: does it add roots or replace them?
- **What happens without NTP.** Some networks block UDP port 123. The console's own clock is often wrong: when the clock capacitor runs flat, it resets to a date in 2001.
- **Randomness.** TLS needs unpredictable random numbers for its key exchange. A PC gets them from the operating system. The Xbox has neither a hardware random-number generator nor an OS generator that nxdk exposes. If the random numbers can be guessed, so can the session keys, and with them the traffic, including a login token or (in M8) a password.

## Decision

1. **BearSSL is a pinned submodule** at `third_party/bearssl`, on upstream master commit `7bea48e`: v0.6 plus later fixes. It's built into libxcat on the host (CMake) and into the XBE (`app/Makefile`). Dependabot can't track it. Upgrades are a dedicated PR, checked against upstream and the CVE feeds at each release (ADR 0005).
2. **`tls` is an `xc_net` wrapped around an `xc_net`.** `xc_tls_net` takes the plain TCP `xc_net`. Its `connect` opens the TCP connection and completes the handshake, sending SNI for the URL's host and checking the certificate against that host. `send`, `recv` and `close` go through BearSSL. It plugs into the HTTP client's `xc_http_config.tls`, so the client doesn't change. Its buffers (the ~33 KB record buffer and the engine state) are given to it at job start. Settings:
   - TLS 1.2 only.
   - Key exchange with ECDHE only, so every session has forward secrecy.
   - AEAD ciphers only: ChaCha20-Poly1305 first, then AES-GCM. No CBC, no RC4, no 3DES.
   - **There is no way to skip validation.** There's no flag, setting or build option for it.
3. **The CA bundle is curl's `cacert.pem`**: Mozilla's root store, filtered to roots trusted for websites, converted to PEM by the curl project (MPL-2.0). A copy is committed at `third_party/cacert/cacert.pem`. Its date and SHA-256 are recorded in `THIRD_PARTY.md`. The **full set** is compiled in: a build-time script checks the file against its pinned SHA-256 (`cacert.pem.sha256`) and embeds the PEM text, which the app decodes at startup with the same loader as the override file in §4, so both take one tested, fuzzed path. A dedicated PR updates it, and each release notes the bundle's date.
4. **`E:\Apps\xcat\cacert.pem` replaces the built-in set; it doesn't add to it.** That way a user can drop a root they distrust, or trust their own CA for a LAN HTTPS server. Limits: 1 MiB and 512 certificates. If the file exists but is unreadable, malformed, or has no usable certificate, HTTPS fails with a message that names the file. It never silently falls back to the built-in set.
5. **Time.** Before the first TLS connection after the app starts, an SNTP client (RFC 4330, client mode) asks one server: `pool.ntp.org` by default, changeable in settings. The answer must:
   - come from the address the request went to;
   - be a server reply (mode 4) with stratum 1–15 and no "alarm" leap indicator;
   - echo the random transmit timestamp the request sent.

   The result is kept as an offset from the monotonic clock. **The console's own clock is never written.**
   - **If NTP fails** (owner decision, 2026-10-15): the console's clock is used instead, but only if it isn't earlier than the XBE's build time. A console reset to 2001 is caught that way. If neither source is acceptable, HTTPS fails with "Couldn't get the correct time (NTP server unreachable and the console's clock is wrong)".
   - The same build-time floor applies to an NTP answer.
   - BearSSL checks every certificate's dates against whichever time was chosen. A wrong time therefore makes a valid certificate fail; it never makes an invalid one pass. The log says which time source was used.
6. **Randomness.** BearSSL's HMAC-DRBG (SHA-256) is seeded through a new platform hook, `xc_entropy`. TLS refuses to start until the hook has supplied its minimum seed.
   - **PC:** the operating system's generator (`BCryptGenRandom`, `getrandom`).
   - **Xbox:**
     - the low bits of the CPU's timestamp counter, sampled at events whose timing the console doesn't control: received network packets (DHCP, DNS, NTP), disk reads and writes, and controller polls;
     - mixed into a SHA-256 pool together with the console's clock, its MAC address, and a seed file, `E:\Apps\xcat\random-seed`.
     - The seed file is replaced with fresh generator output each time the app starts. Fresh random numbers keep coming, even on a console whose state an attacker knows.
     - Each timing sample is counted as at most one bit of entropy. The exact sampling and the minimum are in the `tls` module's header and get tests.

## Consequences

- HTTPS needs no change in the HTTP client, the install or the app's flow: whatever creates the client passes a `tls` net. Only `tls.c` and `hash.c` call BearSSL (ADR 0005).
- The full root set costs roughly 100–150 KB in the XBE. A user can trim or extend it with the override file without a new build.
- SNTP isn't authenticated. An attacker on the path can move the clock, so a certificate that has expired would pass its date check. Using it would still need that certificate's private key. The console-clock fallback carries the same exposure, so it weakens nothing. A clock that is wrong but still inside a certificate's validity period goes unnoticed, which is harmless.
- Networks that block NTP still get HTTPS when the console's clock is roughly right. The M5 exit test's "wrong clock rejected" is tested both ways: a console clock reset to 2001 with NTP unreachable, and a test time outside the certificate's validity.
- The Xbox's randomness is the weakest link in this design. It's documented, logged at startup (which sources contributed) and reviewed again before the beta, when the password crosses the console for on-box login ([ADR 0010](0010-on-box-login-required-for-beta.md)).
- New files on the HDD: `E:\Apps\xcat\cacert.pem` (optional, the user's) and `E:\Apps\xcat\random-seed` (the app's). Neither holds a secret worth stealing on its own.
- M5 also starts whole-image hashing ([ADR 0004](0004-streaming-install.md) §5, owner decision 2026-10-15). Trimmed xisos get their MD5 and SHA-1 computed, with the hash state saved in the journal so a resume keeps it. They're compared when an expected value is given (`xcat-cli`) and logged otherwise. From M6, the archive.org metadata supplies the expected values. This is recorded here because it's part of M5's scope, not a TLS decision.
