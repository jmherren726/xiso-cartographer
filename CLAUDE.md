# xiso-cartographer: agent guide

- Read docs/HANDOFF.md first (and docs/ARCHITECTURE.md once it exists).
- Build/test (host): cmake --preset host-debug && ctest --preset host-debug (from MR-2 onwards; there is no build yet).
- Format: clang-format -i on changed files; CI fails on diffs.
- Parsers live in core/, never in app/. core/ must not include nxdk headers.
- Public symbols use the `xc_` prefix; public headers live in core/include/xcat/.
- Every MR updates docs and CHANGELOG.md as described in CONTRIBUTING.md.
- Hard-to-reverse decisions get an ADR in docs/adr/.
- Never add game images, download links, ROMs or BIOS files to the repo.
- Never implement captcha/bot-protection bypasses.
- Never disable TLS certificate validation.
