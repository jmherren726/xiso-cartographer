# SNTP client (time for certificate checks)

**Status:** Normative. §2–§6 are implemented by `xcat/sntp.h` (M5) and used by the console's network test and `xcat-cli ntp`; the app's HTTPS installs use it from M5 step 8. The reasons are in [ADR 0012 §5](../adr/0012-tls-trust-time-and-entropy.md).

HTTPS checks each certificate's dates, so the console needs the current time, and its own clock can't be trusted (a flat clock capacitor sends it back to 2001). Before the first TLS connection after startup, the client asks one time server with SNTP, the simple client mode of NTP ([RFC 4330](https://www.rfc-editor.org/rfc/rfc4330), packet format of [RFC 5905 §7.3](https://www.rfc-editor.org/rfc/rfc5905#section-7.3)). This document fixes the subset we use and every check an answer must pass.

The words **must**, **must not**, **should** and **may** are used as in RFC 2119.

## 1. Server

- The default server is `pool.ntp.org`, UDP port **123**. A setting may name another host (name or IPv4 literal) and port.
- The client asks **one** server. It doesn't compare servers, and it never writes the console's clock: the result is kept as an offset from the monotonic clock (§5).

## 2. Request

One UDP datagram of exactly **48 bytes**, all zero except:

| Byte(s) | Field | Value |
|---|---|---|
| 0 | LI (bits 7–6), VN (bits 5–3), Mode (bits 2–0) | LI 0, VN **4**, Mode **3** (client): `0x23` |
| 40–47 | Transmit Timestamp | **8 random bytes** (the nonce) |

The nonce comes from the TLS entropy source ([ADR 0012 §6](../adr/0012-tls-trust-time-and-entropy.md)), fresh for every request. The client never sends its own time (RFC 4330 §6 allows this), so the request reveals nothing about the console's clock, and a forged answer must guess 64 random bits to be accepted.

## 3. Timestamps

An NTP timestamp is 64 bits, big-endian: 32 bits of seconds since 1900-01-01 00:00 UTC, then 32 bits of binary fraction.

- **Era (RFC 4330 §3).** If the seconds' top bit is **1**, the time is in 1968–2036 (era 0); if it is **0**, it's in 2036–2104 (era 1, seconds + 2³²). So Unix seconds = NTP seconds − 2,208,988,800, plus 2³² when the top bit is 0.
- Milliseconds = fraction × 1000 / 2³², rounded down.
- A timestamp of all zeros means "not set".

## 4. Checks on the answer

The client waits for one datagram from the server's address (a datagram from any other address or port is ignored). The answer is **accepted only if all** of these hold; otherwise that attempt fails:

1. It is at least **48 bytes**. Bytes after the 48th (extension fields, a MAC) are ignored.
2. **Mode** is 4 (server).
3. **VN** is 3 or 4.
4. **LI** isn't 3 (the server's clock isn't synchronized).
5. **Stratum** is 1–15. Stratum 0 is a *kiss-o'-death*: the Reference ID (bytes 12–15) holds four ASCII letters such as `RATE` or `DENY`, which the client reports. Stratum 16 or more means unsynchronized.
6. **Originate Timestamp** (bytes 24–31) equals the request's nonce, byte for byte.
7. **Transmit Timestamp** (bytes 40–47) isn't zero.
8. The resulting time (§5) isn't earlier than the **floor**: the build time of the program (ADR 0012 §5). An earlier answer is wrong or forged.

The Receive Timestamp (bytes 32–39) is used for the delay correction in §5; when it is zero or later than the Transmit Timestamp, the server's processing time counts as zero. Root delay, root dispersion and the poll and precision fields aren't checked.

## 5. Result

- `rtt` is the time from sending the request to receiving the answer, on the monotonic clock. `server` = Transmit − Receive Timestamp (0 if not usable, see §4). The one-way delay is `max(rtt − server, 0) / 2`.
- The time at the moment the answer arrived = Transmit Timestamp + the one-way delay, in Unix milliseconds. It is stored with the monotonic time of that moment; the current time is then `stored + (monotonic now − monotonic then)`.
- Certificate checks need seconds; the delay correction is there so a slow answer can't move the time by more than half its round trip.

## 6. Attempts and failure

- The client makes up to **3 attempts**, each with a new nonce and its own timeout (default **2 s**): UDP datagrams can be lost. An answer that fails §4 ends that attempt; a kiss-o'-death or an answer below the floor ends the query (asking again won't help).
- A name that doesn't resolve ends the query at once.
- **If the query fails** (ADR 0012 §5): the console's own clock is used, but only if it isn't earlier than the floor. If that is earlier too, HTTPS fails with "Couldn't get the correct time (NTP server unreachable and the console's clock is wrong)".
- The log says which source was used: NTP (server, stratum, round trip, and how far the console's clock is off) or the console's clock (and why NTP failed).

## 7. Security

SNTP isn't authenticated. An attacker on the path who sees the request can answer with the nonce and any time at or above the floor, which could let an expired certificate pass its date check. Using that certificate would still need its private key. ADR 0012 §5 accepts this: the console-clock fallback has the same exposure. A time below the floor is always rejected (§4.8).
