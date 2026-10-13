# Contributing

Thanks for helping. Read [docs/HANDOFF.md](docs/HANDOFF.md) first: it explains what the project is, how it is structured and the coding standards (§7).

## Ground rules

- **No copyrighted content.** Never add game images, download links, ROMs, BIOS/MCPX dumps, XDK material or secrets to the repo, issues, fixtures or CI. Test data is generated from dummy content by `tools/fixtures/`.
- **No bypasses.** Code that gets past captchas, bot protection or access controls, or that violates a source's terms, will not be merged.
- **nxdk only.** The leaked Microsoft XDK is never used.

## Setup

The toolchain arrives with MR-2 (host build) and MR-4 (nxdk build). This section will list the prerequisites and commands then.

## Merge requests

"MR" means a merge request, called a pull request on GitHub.

- **Small and focused:** one concern per MR, ideally under ~400 changed lines (generated files excluded). Split refactors from behaviour changes.
- **Branch names:** `<type>/<short-description>`, e.g. `feat/xdvdfs-reader`, `fix/http-redirect-loop`.
- **Titles:** [Conventional Commits](https://www.conventionalcommits.org/), e.g. `feat(core): …`, `fix(net): …`, `docs: …`, `chore(ci): …`. MRs are squash-merged with the MR title as the commit message.
- **Docs in the same MR.** If an MR changes behaviour, a format, a build step, a public header or a user-visible message, update the matching doc in that MR (README, `docs/ARCHITECTURE.md`, `docs/formats/*`, `docs/testing.md`, `CLAUDE.md`). Reviewers reject MRs that leave docs stale.
- **CHANGELOG entry** under "Unreleased" in [CHANGELOG.md](CHANGELOG.md) for every user-visible change, following [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
- **Tests in the same MR.** New logic comes with unit tests. Bug fixes come with a test that failed before the fix. Platform changes record the xemu/hardware result in the MR description.
- **CI must be green.** Never disable or skip a test to merge. A flaky test is a bug: fix it, or open an issue that blocks the next release.
- **Decisions get an ADR.** Any choice that is hard to reverse (dependency, format, protocol, licence) goes in `docs/adr/NNNN-title.md`. See [ADR 0001](docs/adr/0001-record-architecture-decisions.md).

### Review checklist

The [MR template](.github/pull_request_template.md) contains this list:

- [ ] Tests added or updated, CI green
- [ ] Docs updated, or "no doc impact" stated
- [ ] CHANGELOG updated
- [ ] No copyrighted content, links, ROMs or secrets
- [ ] Format/version changes are backward compatible or bump the version
- [ ] Hardware/xemu test recorded (if platform-affecting)

## Versioning

SemVer tags `vX.Y.Z`. Before 1.0, minor bumps may break things. The map, catalog and journal formats each carry their own version number, documented in `docs/formats/`.

## Third-party code

Third-party code comes in as pinned git submodules under `third_party/`, or vendored with its licence file intact, and is listed in `THIRD_PARTY.md`. Upgrades go in a dedicated MR.

## Licence

By contributing, you agree that your contributions are licensed under the [MIT License](LICENSE). Don't copy in code under GPL or other copyleft licences. See [ADR 0002](docs/adr/0002-license.md).
