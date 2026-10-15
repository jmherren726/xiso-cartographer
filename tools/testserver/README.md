# Range test server

`rangeserver.py` is a small HTTP/1.1 file server (Python standard library only) that behaves like the servers [docs/formats/http.md](../../docs/formats/http.md) expects: single byte ranges answered with `206` and `Content-Range`, keep-alive, `ETag` and `Last-Modified`. Query parameters on any URL make it misbehave in controlled ways, so the client's retries, resume and error handling can be tested against real sockets. It serves only the generated test files you point it at. Never put game images behind it in the repo or CI ([ADR 0006](../../docs/adr/0006-no-links-no-bypass.md)).

```sh
python3 tools/testserver/rangeserver.py --root <dir>                      # http://127.0.0.1:8080/
python3 tools/testserver/rangeserver.py --root <dir> --bind 0.0.0.0       # reachable from the LAN (the Xbox)
xcat-cli get http://127.0.0.1:8080/<file> <out>                           # download it
xcat-cli get "http://127.0.0.1:8080/<file>?drop=100000&times=3" <out>     # survives three dropped connections
```

## Faults

| Parameter | Effect |
|---|---|
| `redirect=N` | Answer `302` N times in a row (relative `Location`) before serving |
| `auth=VALUE` | Answer `401` unless the request carries `Authorization: VALUE` (for credentials tests) |
| `status=CODE` | Answer `CODE` (e.g. `503`, `429`, `403`) with an empty body instead of the file |
| `retry_after=S` | With `status`: also send `Retry-After: S` |
| `drop=N` | Close the connection after N body bytes, without warning |
| `stall=MS`, `at=N` | Stop sending for MS ms after N body bytes (default 0), then close. Use a shorter client `--timeout` |
| `times=K` | How many responses `status`, `drop` and `stall` affect (default 1; `0` = every response) |
| `norange=1` | Ignore `Range` and answer `200` with the whole file |
| `chunked=1` | Send `206` bodies with `Transfer-Encoding: chunked` |
| `change=1` | Report a new `ETag` after the first response, as if the file changed |
| `close=1` | Send `Connection: close` on every response (no keep-alive) |
| `rate=BPS` | Limit bodies to BPS bytes per second |

A fault counts down only on the responses it actually affects: `drop=1000` skips the 1-byte probe (`Range: bytes=0-0`) and hits the first body longer than 1000 bytes. Counters are per URL (ignoring `redirect`) and last as long as the server runs.

## Console test file

`make_test_file.py --out <dir> [--size 1G]` writes `xcat-test-<bytes>.bin`: pseudo-random bytes from a fixed seed, so a given size always gives the same file and CRC-32 on every machine. It prints the `url=`, `size=` and `crc32=` lines for the console's network test (`app/net-test`, [docs/testing.md](../../docs/testing.md#m3-network-download-test)). `xcat-cli get` prints the CRC-32 of what it downloaded too, for comparison.

## Tests

`test_get.py` runs one scenario per CTest test (`get_<scenario>`, e.g. `ctest --preset host-debug -R get_`). Each starts the server in-process on a free port, serving freshly generated pseudo-random files, runs `xcat-cli get` and checks the exit status, the messages, and that the download is byte-identical to the served file, or that a failed download left no file behind. A few also check the requests the client sent (headers, the resumed range, one kept-alive connection). `python3 tools/testserver/test_get.py --list` prints the scenarios.

`test_install.py` does the same for `xcat-cli install` (`install_<scenario>`), with the fixture images (tools/fixtures) as the served images:
- the xiso and full-disc fixtures install over HTTP and match their manifests;
- a local image installs too, and a title comes from the URL's file name;
- dropped connections are survived;
- an image FATX can't store is refused;
- installing the same game twice uses a numbered second folder.

The resume scenarios kill the installer once its journal has a few `done` lines, as a power cut would stop it, then run it again. It must resume in the same folder, start a new folder when the image changed, and, when a finished file was damaged in between, fail the read-back check and then repair the file on the next run.

`test_map_cli.py` checks `xcat-cli map` with a local include and an HTTP include served by the test server.
