# Release engineering

Graft uses SemVer and publishes GitHub Releases from a single unified workflow that runs in the [`release/**`](https://github.com/AEndrix03/Graft/tree/master/.github/workflows/release.yml) branch family.

## Versioning

The single source of truth is [`VERSION`](../../VERSION). The CMake project reads that file and bakes the value into `graft --version`.

Use SemVer:

- PATCH: bug fixes, docs corrections, packaging fixes.
- MINOR: backward-compatible features and new CLI commands.
- MAJOR: breaking CLI/API/config behavior.

The release workflow accepts **only strict `x.y.z`** in `VERSION`. Pre-release modifiers such as `-alpha`, `-beta`, `-rc.N` cause `prep` to fail immediately — they belong on development branches, not on a release branch. The git tag `v<VERSION>` is created and pushed by the workflow itself.

## Publishing

Releases are triggered by a push to a branch matching `release/**` (e.g. `release/0.1.0`, `release/v0.2.0`). They are **not** triggered by tags and there is no `workflow_dispatch` form — the release branch push is the contract.

### Cutting a release

1. On `develop`, bump [`VERSION`](../../VERSION) and rename `## [Unreleased]` in
   [`CHANGELOG.md`](../../CHANGELOG.md) to `## [x.y.z] - YYYY-MM-DD` (release
   day). Add a fresh empty `## [Unreleased]` above it and update the compare
   links at the bottom. Commit as `chore: release x.y.z`.
2. Cut the release branch from `develop` and push it — this starts the workflow:

   ```bash
   git switch -c release/0.2.0
   git push -u origin release/0.2.0
   ```

3. Fix anything the workflow rejects on the release branch itself.
4. Once the release is published, close it: merge `release/x.y.z` into
   `master` and `develop` with `--no-ff`, pull the `[skip ci]` manifest commit
   the workflow pushed to `master` into `develop`, and delete the release
   branch locally and on `origin`. The tag `vx.y.z` keeps its history.

Hotfixes follow the same steps from a `hotfix/x.y.z` branch cut from the latest
tag, pushed as `release/x.y.z` when ready.

The workflow then runs end-to-end. Each stage is a hard gate — if any fails, no tag is created and no release is published.

| Stage | What it does |
| ----- | ------------ |
| `prep`     | Validates `VERSION` is strict `x.y.z` and that `CHANGELOG.md` has a dated `## [x.y.z] - YYYY-MM-DD` section; refuses to start if another release run is already in progress (auto-cancels itself to prevent overlap). |
| `test`     | Builds with `GRAFT_BUILD_TESTS=ON` and runs `ctest`. Bails out on the first failure. |
| `tarball`  | Builds the Linux x86_64 release artifact via `scripts/package-release.sh`. |
| `bottle`   | Builds the Homebrew bottle for `x86_64_linux` via `brew install --build-bottle`. |
| `scoop`    | Builds the Windows x86_64 zip layout under MSYS2/MinGW64. |
| `publish`  | Downloads all artifacts, generates `SHA256SUMS` + SPDX SBOM, signs everything with keyless Sigstore/cosign, attests build provenance, creates the `v<VERSION>` tag, opens the GitHub Release with this version's `CHANGELOG.md` section as the notes body, and patches `Formula/graft.rb` + `bucket/graft.json` on `master` in a single `[skip ci]` commit. |

Build caching: `third_party/llama.cpp/build` is cached keyed on the submodule commit SHA, and `ccache` is reused across runs. MSYS2 packages are cached by `setup-msys2`.

## Assets

Current shipped assets per release:

- `graft-linux-x86_64.tar.gz` — portable Linux x86_64 build
- `graft-windows-x86_64.zip` — Scoop/Windows x86_64 build
- `graft-<version>.tar.gz` — source tarball used by the Homebrew formula
- `*.bottle.tar.gz` + `*.bottle.json` — Homebrew bottle (currently `x86_64_linux` only)
- `SHA256SUMS` — checksums for everything above
- `graft-sbom.spdx.json` — SPDX SBOM
- `*.sig` and `*.pem` — keyless Sigstore signatures and certificates for every asset and for `SHA256SUMS`

> **Platform coverage today:** Linux x86_64 + Windows x86_64. macOS and aarch64 are intentionally **not** built as prebuilt assets — macOS Homebrew users fall back to `--build-from-source` automatically; aarch64 users build from source until those targets are wired back in.

Release archives are model-free. They update the installed runtime and shared agent integration templates, but they do not overwrite user data:

- profiles and databases under `~/.graft/profiles/`
- `~/.graft/config.yaml`
- downloaded models under `~/.graft/models/`

## Upgrade flow

`graft upgrade` calls the GitHub Releases API, compares the current embedded version with the latest release tag, prompts:

```text
Upgrade graft 0.1.0 -> v0.2.0? [y/N]
```

When confirmed, it downloads the platform asset and `SHA256SUMS`, verifies the asset hash, extracts it, and merges it into the installed graft root.

Useful flags:

```bash
graft upgrade --check
graft upgrade --yes
```

Environment overrides for tests and forks:

```bash
GRAFT_UPGRADE_REPO=owner/repo
GRAFT_UPGRADE_LATEST_URL=https://example.test/latest.json
```
