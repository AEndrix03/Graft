# Changelog

All notable changes to **graft** are documented in this file.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project follows [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added

- The installers write a minimal `config.yaml` (just the paths that depend on the install location) instead of copying the 403-line example. Built-in defaults cover the rest, so tuning improvements in later releases reach existing installs; `config.example.yaml` ships alongside as the documented reference.
- **One-line installers** at the repo root: `install.sh` (Linux/macOS) and `install.ps1` (Windows). They download the prebuilt release archive for the platform, verify it against the published `SHA256SUMS` and refuse to continue on a mismatch, extract into `~/.graft`, fetch the BGE-M3 model once, write `config.yaml` with absolute paths without overwriting an existing one, put `~/.graft/bin` on `PATH`, and run a smoke check. No compiler, no submodules, no MSYS2.
- `graft setup` with no argument now sets up every agent whose config directory exists on the machine, and the installers run it for you, so the only manual step left is `/graft-init` inside the agent.

### Changed

- `graft setup` installs skills and nothing else. All hook installation, `settings.json` / `hooks.json` / `config.toml` merging and instruction-file writing were removed from the binary (~290 lines, plus the shipped hook scripts). Agent wiring is done by `/graft-init` from inside the agent.
- `/graft-init` asks one question (global or project) instead of four, and writes the rule to `CLAUDE.md` + `.claude/rules/graft.md` on Claude Code, or `AGENTS.md` elsewhere.
- The `graft` skill was rewritten around the prompter model: a near hit is useful, a miss is a gap to fill, short search-engine-style queries, `classify` for keywords, verify before trusting a `STRONG`, delete + re-insert for stale nodes, and a one-line end-of-turn recap.
- `scripts/install.sh` / `scripts/install.ps1` are now `scripts/build-from-source.sh` / `scripts/build-from-source.ps1` - they build from source and are for contributors, GPU builds and platforms without a prebuilt archive.

### Fixed

- The daemon's startup failure reason now reaches the user. `graftd` already printed the real cause on stderr ("embed init failed", "storage open failed", "socket listen failed"), but on Windows it was spawned DETACHED with no stdio redirection, so the output was discarded and the CLI only said "socket did not become ready in 20000 ms". The spawned daemon now inherits a handle to the log on both platforms, the log lives at `$GRAFT_HOME/graftd.log` as the docs always claimed (it used to sit next to the binary), and a failed auto-start quotes the tail of it.
- A daemon that exits during startup is detected immediately on Windows instead of after the full 20 s poll: a missing model now reports in about 1 second.

- `ctest` on Windows no longer fails with `0xc0000139` (STATUS_ENTRYPOINT_NOT_FOUND) for the six tests that import llama/ggml directly: CMake now prepends the llama.cpp build directories and the toolchain runtime directory to the test PATH. The CLI was unaffected because the linker drops its unused llama imports, which is what made the failure look like a code problem. 11/11 tests pass.

### Removed

- The three harness hook scripts (`query_inject.js`, `mark_candidate.js`, `propose_memoryze.js`) and `scripts/install-codex-hooks.*`.
- The duplicated per-agent skill copies under `integrations/claude-code/skills/`; `integrations/standard/skills/` is the single source.

## [0.1.0] — Initial release

First public release of graft — local-first agentic memory for AI coding agents.
A single binary + single SQLite file that gives any agent persistent memory across sessions, context resets, and machines, with no cloud and no API key.

### Core engine

- **C11 daemon** (`graftd`) with `AF_UNIX` socket transport and a thin `graft` CLI client; MessagePack wire protocol for low-overhead local IPC.
- **Storage** on SQLite with `sqlite-vec` (dense vector index) and `FTS5` (BM25 lexical index) in a single DB file.
- **Embeddings** via embedded `llama.cpp` running BGE-M3 on CPU out of the box; opt-in GPU acceleration with `GRAFT_GPU=cuda` (NVIDIA) or `GRAFT_GPU=hip` (AMD ROCm 6 / 7).
- **WAL-safe sync layer**: single-writer push half, no double-open, no WAL contention under concurrent reads.

### Retrieval

- **`graft query`** — verified semantic cache returning `STRONG` / `WEAK` / `MISS` in milliseconds. Multi-signal verifier refuses to claim a hit when dense and lexical signals disagree, so agents never quote confidently-wrong answers.
- **`graft retrieve`** — hybrid search fusing dense (BGE-M3 cosine) and lexical (BM25 over title + body) via Reciprocal Rank Fusion.
- **`graft explore`** — beam-search graph walk over keyword and semantic edges with MMR diversity and `gamma^step` decay.

### Knowledge model

- **Memory nodes**: `title` (retrieval anchor) + `body` (full context) + keywords.
- **Graph edges**: keyword and semantic links between nodes, walked by `explore`.
- **Supersession**: replace outdated nodes atomically while keeping the old version visible as `SUPERSEDED` — history stays, mistakes don't propagate.
- **Confidence levels** surfaced to clients (STRONG / WEAK / MISS) so agents can gate behavior on retrieval quality.

### Multi-tenant profiles

- Isolated DBs and sockets per profile (`work`, `personal`, project-scoped); switch with `GRAFT_PROFILE=<name>`.
- Import / export / merge profiles as plain SQLite files — portable, diffable, scriptable.

### Optional REST API + 3D viewer

- Nine JSON endpoints (`/v1/match`, `/v1/search`, `/v1/insert`, …) gated by a flag in `config.yaml`.
- Browser-based 3D graph viewer with click-to-edit and atomic supersession.

### Agent integrations

Each adapter ships **skills** (when to search, when to save) and, where the harness supports them, optional **hooks** (deterministic execution on `UserPromptSubmit` / `PostToolUse` / `Stop` so the model can't forget):

- **Claude Code** — skills + optional hooks.
- **Codex** — skills + optional `AGENTS.md` and hooks.
- **Claude Desktop** — MCP server (stdio).
- **ChatGPT** — MCP server (stdio or HTTP) with optional OAuth gateway.
- **Gemini CLI** — `GEMINI.md` memory file.
- **Open Code** — skills + optional `AGENTS.md`.

### Microservices pattern

- Reference architecture for L1 Redis + L2 graft semantic cache + L3 graft + LLM with writeback, documented in `docs/microservices/`.
- Designed to absorb most "GPT in a microservice" traffic before it hits the LLM, with the system getting cheaper and faster over time as L3 answers write back into L2.

### Packaging & distribution

- **Homebrew formula** (`Formula/graft.rb`) with prebuilt Linux bottle.
- **Scoop bucket** (`bucket/graft.json`) for Windows users.
- **Cross-platform installer** scripts: `scripts/install.sh` (Linux / macOS / MSYS2) and `scripts/install.ps1` (Windows, auto-installs MSYS2).
- Build under 3 minutes on a laptop.

### CI / release pipeline

- Single unified release workflow triggered by pushes to the `release` branch.
- Strict semver gate (only `x.y.z`, no pre-release suffixes).
- `ctest` → parallel build of Linux tarball, Homebrew bottle, Scoop zip → single signed publish job with cosign signatures, SBOM, and build provenance attestations.
- Smart caching of `llama.cpp` build (keyed on submodule SHA) and `ccache` across runs.

### Documentation

- Per-feature docs tree under `docs/` (install, retrieval, insert, profiles, embeddings, integrations, microservices, HTTP API, viewer, maintenance, storage, architecture, release).
- Glossary in `docs/concepts.md`, use cases in `docs/use-cases.md`.
- Contributor guide in `CONTRIBUTING.md` with commit-msg policy (Conventional Commits, English ASCII subject ≤ 70 chars).

[0.1.0]: https://github.com/AEndrix03/Graft/releases/tag/v0.1.0
