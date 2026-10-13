# xiso-cartographer: agent guide

- Read docs/ARCHITECTURE.md and docs/HANDOFF.md first. Formats are specified in docs/formats/ (normative); parsers must follow them and link the section they implement.
- Build/test (host): cmake --preset host-debug && cmake --build --preset host-debug && ctest --preset host-debug
- Clone or update with submodules: git submodule update --init.
- New tests: core/tests/test_<module>.c (Unity) + xcat_add_test() in core/tests/CMakeLists.txt.
- XBE build: make docker-xbe (see docs/build-xbox.md). New core/ sources used by the app must also be added to SRCS in app/Makefile.
- Format: clang-format -i on changed files; CI fails on diffs.
- Parsers live in core/, never in app/. core/ must not include nxdk headers.
- Public symbols use the `xc_` prefix; public headers live in core/include/xcat/.
- Every MR updates docs and CHANGELOG.md as described in CONTRIBUTING.md.
- Hard-to-reverse decisions get an ADR in docs/adr/.
- Never add game images, download links, ROMs or BIOS files to the repo.
- Never implement captcha/bot-protection bypasses.
- Never disable TLS certificate validation.
