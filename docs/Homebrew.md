# Homebrew install

Graft ships a Homebrew formula in `Formula/graft.rb`. It is meant for a
project-owned tap first, not for `homebrew/core` submission yet. There is no
tagged stable release yet, so the formula is **HEAD-only** today — install with
`--HEAD`. Add a `stable` block (tarball URL + sha256 + `version`) and drop the
`livecheck do skip ... end` once the first tag is published.

## Install from the repository tap

```sh
brew tap AEndrix03/graft https://github.com/AEndrix03/Graft.git
brew install --HEAD graft
graft stats
```

The formula builds the CLI and daemon from source, installs the pinned BGE-M3
GGUF model as a checksummed Homebrew resource, and keeps runtime profiles under
`~/.graft`.

## Updating between commits

`brew` caches sources. After a push to `master`, pull the new HEAD with:

```sh
brew upgrade --fetch-HEAD graft   # or: brew reinstall --HEAD graft
brew test graft
```

## Configuration

The formula installs a default config in the Cellar:

```sh
brew --prefix graft
```

Graft will use that config automatically when no user config exists. To
customize settings, copy it once:

```sh
mkdir -p ~/.graft
cp "$(brew --prefix graft)/config.example.yaml" ~/.graft/config.yaml
```

Then edit `~/.graft/config.yaml`. Profiles and sockets remain user-scoped, so
the Homebrew package can be upgraded without touching user data.

## Release process

Releases are tag-driven. Pushing a tag of the form `vX.Y.Z[-suffix]` triggers
`.github/workflows/homebrew-bottle.yml`, which builds bottles on Linux x86_64,
macOS Intel (Ventura) and macOS Apple Silicon (Sonoma) in parallel.

What the workflow does depends on the tag shape:

| Tag shape           | Example          | Bottles built | GitHub Release | `Formula/graft.rb` on master |
| ------------------- | ---------------- | ------------- | -------------- | ---------------------------- |
| `vX.Y.Z` (stable)   | `v0.1.0`         | yes           | created/updated, assets attached | patched: stable url, sha256, version, full `bottle do` block |
| `vX.Y.Z-suffix`     | `v0.1.0-alpha.1` | yes (validation only) | NOT created   | not modified                 |

Pre-release tags (anything with a `-` suffix) only verify that the build works
on all platforms. They neither publish a Release nor update the formula. Users
of pre-release versions stay on `brew install --HEAD graft` until a stable tag
is cut.

### Steps for the maintainer

1. Work on a `release/<version>` (or `hotfix/<version>`) branch.
2. Bump `VERSION` to the target version, e.g. `0.1.0-alpha.1` or `0.1.0`.
3. Merge the branch into `master` (PR or fast-forward).
4. Tag `master` with `v<version>` and push the tag:
   ```sh
   git tag v0.1.0-alpha.1
   git push origin v0.1.0-alpha.1
   ```
5. Watch the workflow under Actions. On stable tags it will:
   - Publish the GitHub Release with bottles + canonical source tarball.
   - Push a follow-up commit on master patching `Formula/graft.rb`
     (`chore(release): publish vX.Y.Z bottles and stable url [skip ci]`).

### Re-running a release

To rebuild bottles for a tag that already exists, use the workflow's
`workflow_dispatch` and pass the tag name as input. The `gh release upload`
step uses `--clobber` so attached assets get overwritten cleanly. If the
formula on master needs to be reset (e.g. you want to drop the bottles for a
yanked release), revert the auto-commit manually.

### Sanity checks before tagging

```sh
ruby -c Formula/graft.rb            # syntax check (needs Ruby installed)
brew install --HEAD ./Formula/graft.rb   # source build still works locally
brew test graft
```

The formula pins third-party source resources by commit and the BGE-M3 model
by Hugging Face commit + SHA256. Keep those reproducible — avoid changing
resources to branch URLs without a pinned `revision:`.
