# Scoop install (Windows)

On Windows, Graft is distributed through a [Scoop](https://scoop.sh) manifest
that lives directly in this repo under [`bucket/graft.json`](../bucket/graft.json).
The asset itself (an `x86_64` binary build produced via MSYS2/MinGW64) is
attached to each stable GitHub Release.

## Install

```pwsh
scoop bucket add graft https://github.com/AEndrix03/Graft
scoop install graft
```

The first install also downloads the BGE-M3 GGUF model (~671 MB) into
`$(scoop prefix graft)\share\graft\models\bge-m3.gguf`. This happens in the
manifest's `post_install` step, so it only runs once per upgrade.

`scoop update graft` picks up new releases automatically thanks to the
`checkver` + `autoupdate` block in the manifest.

## Release process

Same trigger as the Homebrew bottle pipeline: pushing a tag of the form
`vX.Y.Z[-suffix]` to GitHub starts `.github/workflows/scoop-bucket.yml`,
which builds the Windows binary on a `windows-latest` runner inside MSYS2
MinGW64 and packages it as `graft-windows-x86_64.zip`.

| Tag shape           | Example          | Zip built | GitHub Release | `bucket/graft.json` on master |
| ------------------- | ---------------- | --------- | -------------- | ----------------------------- |
| `vX.Y.Z` (stable)   | `v0.1.0`         | yes       | created/updated, asset attached | patched: `version`, `url`, `hash` |
| `vX.Y.Z-suffix`     | `v0.1.0-alpha.1` | yes (validation only) | NOT created      | not modified                  |

Pre-release tags only validate that the Windows build works. The asset is
attached to the workflow run for inspection; the manifest stays at the last
stable version so users on `scoop update` are not pushed to an unstable build.

## Local build (without Scoop)

If you want a Windows build outside CI, run `scripts/install.ps1`. It
delegates to `scripts/install.sh` inside an MSYS2 MinGW64 shell and installs
to `~/.graft/bin`.

## Why a personal bucket and not the official one

The official `ScoopInstaller/Main` bucket has a review process geared toward
mature releases. Until Graft has a stable `v1.0.0` and is happy to bind to
Scoop's stricter rules (no `post_install` model downloads, strict naming,
etc.), the personal bucket is friction-free and keeps the manifest co-located
with the source.
