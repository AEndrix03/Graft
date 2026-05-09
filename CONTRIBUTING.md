# Contributing

Thanks for considering a contribution. memgraph is a small C / CMake project and the bar for changes is straightforward.

## Setup

```bash
git clone https://github.com/AEndrix03/lmemorygraph.git && cd lmemorygraph
bash scripts/install.sh        # Linux, macOS, Windows MSYS2
pwsh scripts/install.ps1       # Windows (auto-installs MSYS2 if needed)
```

The installer pulls submodules, builds llama.cpp (CPU by default; pass `MEMGRAPH_GPU=cuda|hip` for GPU), downloads BGE-M3 (~600 MB), builds `memgraph` + `memgraphd`, and activates the commit-msg hook described below. See the [README](./README.md#install) for manual steps.

## Build and test

```bash
cmake --build build              # incremental build
ctest --test-dir build           # run the suite
./build/test_<name>              # run a single test
```

## Project layout

- `src/` — daemon, CLI, retrieval, embed, storage, config (one subdir per concern)
- `include/memgraph/` — public C headers
- `tests/` — `test_*.c` files; CMake auto-registers each one
- `integrations/` — per-agent adapters (skills, AGENTS.md files, MCP server, hooks)
- `scripts/` — installers and git hooks
- `third_party/` — submodules (llama.cpp, sqlite-vec, mpack, BLAKE3)

## Commit format

The installer activates `scripts/git-hooks/commit-msg` via `core.hooksPath`. Every commit is checked against:

- **Conventional Commits**: `<type>(<scope>)?!?: <description>`
- **Subject only**, no body, no `Co-Authored-By:` trailer
- **Total length ≤ 70 characters**
- **ASCII only** (proxy for "write in English")

Allowed types: `feat`, `fix`, `chore`, `docs`, `style`, `refactor`, `test`, `perf`, `build`, `ci`, `revert`.

```
feat(query): cap MISS fallback at 5 nodes
fix(embed): respect hardware_accel=false on CPU-only builds
docs: link integrations README
```

If you cloned without running the installer, enable the hook manually:

```bash
git config core.hooksPath scripts/git-hooks
```

## Pull requests

- Branch from `master`; open the PR against `master`.
- Keep each PR focused on one concern.
- For non-trivial changes, open an issue first to align on direction.
- Update `README.md`, `CONTRIBUTING.md`, or the relevant `integrations/*/README.md` when user-facing behavior changes.

## Reporting issues

Use GitHub Issues. Include:

- Platform (OS, arch, shell)
- `memgraph stats` output, if relevant
- Steps to reproduce, expected vs observed
- Daemon logs from `~/.lmemorygraph/memgraphd.{out,err}.log` when applicable

## Code of conduct

Be civil. Argue ideas, not people. That is the whole policy.
