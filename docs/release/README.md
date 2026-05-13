# Release engineering

Graft uses SemVer and publishes GitHub Releases from signed tags.

## Versioning

The single source of truth is [`VERSION`](../../VERSION). The CMake project
reads that file and bakes the value into `graft --version`.

Use SemVer:

- PATCH: bug fixes, docs corrections, packaging fixes.
- MINOR: backward-compatible features and new CLI commands.
- MAJOR: breaking CLI/API/config behavior.

The release tag must be `vX.Y.Z` and must match `VERSION`.

## Publishing

1. Update `VERSION`.
2. Update docs/changelog content as needed.
3. Commit with a Conventional Commit subject.
4. Tag the commit:

   ```bash
   git tag vX.Y.Z
   git push origin master vX.Y.Z
   ```

The `release` workflow builds platform assets, generates `SHA256SUMS`, signs
assets with keyless Sigstore/cosign, writes an SPDX SBOM, generates release
notes, and publishes the GitHub Release.

You can also run the workflow manually with a `version` input. The workflow
will create and push the matching tag if it does not already exist.

## Assets

Expected asset names:

- `graft-linux-x86_64.tar.gz`
- `graft-linux-aarch64.tar.gz`
- `graft-macos-x86_64.tar.gz`
- `graft-macos-arm64.tar.gz`
- `graft-windows-x86_64.zip`
- `SHA256SUMS`
- `graft-sbom.spdx.json`
- `*.sig` and `*.pem` signature/certificate files

Release archives are model-free. They update the installed runtime and shared
agent integration templates, but they do not overwrite user data:

- profiles and databases under `~/.graft/profiles/`
- `~/.graft/config.yaml`
- downloaded models under `~/.graft/models/`

## Upgrade flow

`graft upgrade` calls the GitHub Releases API, compares the current embedded
version with the latest release tag, prompts:

```text
Upgrade graft 0.1.0 -> v0.2.0? [y/N]
```

When confirmed, it downloads the platform asset and `SHA256SUMS`, verifies the
asset hash, extracts it, and merges it into the installed graft root.

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
