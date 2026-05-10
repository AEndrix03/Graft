# Homebrew install

Graft ships a Homebrew formula in `Formula/graft.rb`. It is meant for a
project-owned tap first, not for `homebrew/core` submission yet. The tap formula
currently tracks `master`; switch the formula URL to a tagged tarball before a
public stable release.

## Install from the repository tap

```sh
brew tap AEndrix03/graft https://github.com/AEndrix03/Graft.git
brew install graft
graft stats
```

The formula builds the CLI and daemon from source, installs the pinned BGE-M3
GGUF model as a checksummed Homebrew resource, and keeps runtime profiles under
`~/.graft`.

## Development install

Use `--HEAD` when testing unreleased changes from `master`:

```sh
brew reinstall --HEAD graft
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

## Release checklist

Before publishing a tagged release for the tap:

```sh
ruby -c Formula/graft.rb
brew audit --strict --online Formula/graft.rb
brew install --build-from-source Formula/graft.rb
brew test graft
```

The formula intentionally pins third-party source resources by commit and pins
the model URL to a Hugging Face repository commit plus SHA256. Avoid changing
those resources to branch URLs; Homebrew installs should be reproducible. For a
stable release, also pin Graft itself to a tag/tarball and fill in the release
archive SHA256.
