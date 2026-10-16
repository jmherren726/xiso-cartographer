# CA bundle

`cacert.pem` is the built-in set of TLS trust anchors ([ADR 0012](../../docs/adr/0012-tls-trust-time-and-entropy.md) §3): Mozilla's root store, filtered to the roots Mozilla trusts for websites and converted to PEM by the curl project ([curl.se/docs/caextract.html](https://curl.se/docs/caextract.html)). It's vendored unmodified, under the MPL-2.0 like the data it comes from.

- Current file: Mozilla data as of Fri Sep 25 03:12:01 2026 GMT, 121 certificates.
- `cacert.pem.sha256` pins its SHA-256. The build (`tools/cacert/embed_cacert.py`) refuses a file that doesn't match, so the bundle only changes in a PR that also changes the pin.

## Updating (a dedicated PR)

1. Download `https://curl.se/ca/cacert.pem` and `https://curl.se/ca/cacert.pem.sha256`, and check that they match (`sha256sum cacert.pem`).
2. Replace `cacert.pem`, write the new hash into `cacert.pem.sha256`, and update the date and count above and in `THIRD_PARTY.md`.
3. Build and run the tests (`test_tls` decodes the whole bundle), and note the bundle's date in `CHANGELOG.md`.

The console can use a different set without a new build: `E:\Apps\xcat\cacert.pem` replaces this one (§4).
