# Third-party code

Every dependency is a pinned git submodule under `third_party/` (or vendored with its licence file intact). Licence compatibility is covered by [ADR 0002](docs/adr/0002-license.md). Upgrades go in a dedicated MR.

| Component | Version | Licence | Path | Used for | Ships in release |
|---|---|---|---|---|---|
| [Unity](https://github.com/ThrowTheSwitch/Unity) | v2.6.1 | MIT | `third_party/unity` | Host unit tests ([ADR 0003](docs/adr/0003-unit-test-framework.md)) | No |
