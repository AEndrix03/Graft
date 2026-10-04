# Contributing

Thanks for considering a contribution. graft is a small C / CMake project and the bar for changes is straightforward.

## Setup

```bash
git clone https://github.com/AEndrix03/graft.git && cd graft
bash scripts/build-from-source.sh        # Linux, macOS, Windows MSYS2
pwsh scripts/build-from-source.ps1       # Windows (auto-installs MSYS2 if needed)
```

The installer pulls submodules, builds llama.cpp (CPU by default; pass `GRAFT_GPU=cuda|hip` for GPU), downloads BGE-M3 (~600 MB), builds `graft` + `graftd`, and activates the commit-msg hook described below. See the [README](./README.md#install) for manual steps.

## Build and test

```bash
cmake --build build              # incremental build
ctest --test-dir build           # run the suite
./build/test_<name>              # run a single test
```

## Project layout

- `src/` — daemon, CLI, retrieval, embed, storage, config, http (one subdir per concern)
- `include/graft/` — public C headers
- `tests/` — `test_*.c` files; CMake auto-registers each one and links it against `graft_cli` (every CLI subcommand except `main()`, on top of `graft_core`), so CLI code is testable too
- `plugins/graft/` — the agent plugin: the skills, plus the Claude Code and Codex manifests
  (the marketplaces are `.claude-plugin/` and `.agents/plugins/` at the repo root)
- `integrations/` — per-agent adapters (AGENTS.md files, MCP server, optional hooks)
- `viewer/` — Vue 3 + Vite + three.js SPA served by the daemon's HTTP layer
- `docs/` — extended docs (HTTP API reference, etc.)
- `scripts/` — installers and git hooks
- `third_party/` — submodules (llama.cpp, sqlite-vec, mpack, BLAKE3)

## Viewer

The 3D graph viewer is independent of the C build:

```bash
cd viewer
npm install
npm run build      # static bundle in viewer/dist/
npm run dev        # hot-reload dev server, proxies /v1/* to :9977
```

The daemon serves `viewer/dist/` at `/` when `http.enabled: true`. See [`viewer/README.md`](./viewer/README.md) and [`docs/HTTP-API.md`](./docs/HTTP-API.md).

## Commit format

The installer activates `scripts/git-hooks/commit-msg` via `core.hooksPath`. Every commit is checked against:

- **Conventional Commits**: `<type>!?: <description>`. Write it without a scope: the hook still tolerates `(<scope>)`, but the project does not use it, because the type and the description already say what changed
- **Subject only**, no body, no `Co-Authored-By:` trailer
- **Total length ≤ 70 characters**
- **ASCII only** (proxy for "write in English")

Allowed types: `feat`, `fix`, `chore`, `docs`, `style`, `refactor`, `test`, `perf`, `build`, `ci`, `revert`.

```
feat: cap the MISS fallback at 5 nodes
fix: respect hardware_accel=false on CPU-only builds
docs: link integrations README
```

If you cloned without running the installer, enable the hook manually:

```bash
git config core.hooksPath scripts/git-hooks
```

## Branching model

Graft follows a lightweight git-flow:

| Branch | Purpose |
| ------ | ------- |
| `master` | What is released. Only receives merges from `develop` (at release time) and hotfixes. |
| `develop` | Integration branch. Every feature and fix lands here first. |
| `feat/<topic>`, `fix/<topic>` | Short-lived work branches, cut from `develop`. |
| `release/<x.y.z>` | Pushing one triggers the release workflow. Deleted once merged back; the `v<x.y.z>` tag keeps it. |
| `hotfix/<x.y.z>` | Urgent fix cut from the latest tag, released as `release/<x.y.z>`, then merged into `master` and `develop`. |

See [`docs/release/`](./docs/release/README.md) for the release procedure.

## Changelog

Every user-visible change adds a line to [`CHANGELOG.md`](./CHANGELOG.md) under
`## [Unreleased]`, in the same PR, in the right group (`Added`, `Changed`,
`Deprecated`, `Removed`, `Fixed`, `Security`). Write it for users: what changed
and why it matters, not which files moved. Internal refactors, tests and CI
tweaks do not need an entry.

## Pull requests

- Branch from `develop`; open the PR against `develop`.
- Keep each PR focused on one concern.
- For non-trivial changes, open an issue first to align on direction.
- Update `README.md`, `CONTRIBUTING.md`, or the relevant `integrations/*/README.md` when user-facing behavior changes.

## Reporting issues

Use GitHub Issues. Include:

- Platform (OS, arch, shell)
- `graft stats` output, if relevant
- Steps to reproduce, expected vs observed
- Daemon logs from `~/.graft/graftd.{out,err}.log` when applicable

## Code of conduct

Be civil. Argue ideas, not people. That is the whole policy.
