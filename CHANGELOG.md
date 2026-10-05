# Changelog

All notable changes to **graft** are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Every change that a user could notice lands here in the same pull request that
makes it, under `## [Unreleased]

### Changed

- The MCP server's instructions, the `graft_delete` description, the ChatGPT / Claude.ai prompt snippets and the `learn` skill no longer recommend "delete, then re-insert" to fix a note. A correction is now an insert followed by `graft_maintain_resolve` `supersede`; doubtful notes go `stale`, useless ones `retire`, all audited and reversible. `graft_delete` is described as the permanent, unaudited hard delete it is, for content that must vanish such as a leaked secret ([#18](https://github.com/AEndrix03/Graft/issues/18)).
- `query`, `retrieve` and `explore` are read-only again: they still hide expired notes but no longer delete them on the way, so a search never opens a write transaction on the shared database. Expired notes are removed by `graft consolidate` / `graft maintain apply-safe` ([#19](https://github.com/AEndrix03/Graft/issues/19)).

### Fixed

- A note marked `stale` could come back as a `STRONG` hit looking exactly like a live one, and the Claude Code prompt hook injected it without a word. `query` (and its `--explain` candidates), `retrieve` and `explore` now report each result's `state`, over the CLI, HTTP and MCP alike; the prompt hook (and the optional `query_inject.js` hook) labels a stale note and tells the agent to verify it before relying on it. Stale notes stay searchable, as documented ([#17](https://github.com/AEndrix03/Graft/issues/17)).
- Inserting the exact content of a superseded note no longer looks like an ordinary successful duplicate. The insert response now carries the node's `state`, and for a superseded match also `superseded_by`, the note that replaced it; the old note stays superseded (restoring it would break its lineage). A retired match is still restored and reported `active` ([#20](https://github.com/AEndrix03/Graft/issues/20)).

## [0.2.0] - 2026-10-04

Graft now looks after itself: the agent bootstraps a project's memory, keeps it in sync with the files it came from, and maintains it without asking you. Install from the Claude Code or Codex marketplace, and eight bugs found in review are fixed.

### Added

- **Zero-touch lifecycle.** After `/graft-init` the agent runs graft on its own: the rule it writes (and the `graft` skill's new *Lifecycle* section) has it bootstrap a project progressively the first time it meets it, recall before substantial work, supersede a note the code contradicts on the spot, save durable knowledge after the work (with `--source file:` when it comes from a file), and do maintenance only when it is due and only a bounded batch, without asking the user in the normal path. The new `graft status [--root DIR]` gives it everything in one cheap JSON (well under a second, never starts the daemon): the project's bootstrap state, `maintain status` when a daemon is running, the changed / removed sources as of the last scan, and a `next` list of housekeeping steps (`bootstrap`, `apply-safe`, `resolve`, `refresh-sources`, `scan`) with priorities. The Claude Code plugin adds two hooks run by the binary itself (`graft hook session-start|prompt`): a one-line housekeeping hint at session start, and the note of a `STRONG` match injected as context on each prompt. Both never start the daemon, stay silent on any failure, and can be turned off with `GRAFT_HOOKS=0` or `GRAFT_HOOK_PROMPT=0` ([#6](https://github.com/AEndrix03/Graft/issues/6)).
- **Project bootstrap.** `/learn bootstrap [path]` builds a new project's memory unattended: no plan to approve, no node cap to choose. It maps the project into topics (docs, build manifests, configuration, public contracts, the implementation, optionally the local commit history), ranks them by importance with the area of the current task first, and spends a bounded budget per run on distilled knowledge nodes (decisions, invariants, module boundaries, procedures, gotchas, contracts), never file summaries, generated code or secrets, with keywords reconciled against the graph, duplicates skipped and every node linked to its files with `--source`. Re-runs only revalidate what changed and continue with the pending topics. The coverage state lives in the new CLI-only `graft project status|mark|reset` (topic map, runs, files backing nodes), which never starts the daemon ([#3](https://github.com/AEndrix03/Graft/issues/3)).
- **Autonomous maintenance.** `graft maintain` splits graph upkeep between Graft and the agent. `apply-safe` does the mechanical part on its own: the `consolidate` pass, collapsing exact duplicates (same title and body, oldest kept, the others superseded), purging retired nodes after a retention window, and recording the run. `scan` reports what needs judgment as candidates with deterministic ids, evidence and suggested actions (near duplicates, possible supersessions, contradictions, file sources that changed or disappeared, keyword spelling variants, isolated never-read notes) without touching the graph; it re-hashes the project's file sources like `sources diff`, and its work is bounded (`--limit`, `maintenance.*` knobs). The agent checks each candidate against the current code and applies one narrow action with `resolve` (`keep`, `stale`, `retire`, `restore`, `supersede`, `merge`, `refresh`, ...). `keep` hides a candidate until its evidence changes, so repeated scans converge. `retire` is a reversible soft delete: retired nodes vanish from `query` / `retrieve` / `explore` and can be restored until `maintenance.retention_days` (default 30) elapse. Every resolution goes to an audit log (`graft maintain log`), and `graft maintain status` gives agents a cheap view of pending work and inserts since the last run. `graft get` now shows the node's `state`. The `memory-audit` skill drives this loop, and the MCP server exposes it as `graft_maintain_*` tools. ([#5](https://github.com/AEndrix03/Graft/issues/5))
- **Memory provenance.** `graft insert --source <locator>` (repeatable: `file:<path>`, `url:<url>`, `conversation`, `manual`) records where a memory comes from. File sources are stored relative to their project root with a BLAKE3 fingerprint and a project id (the normalized `origin` remote, else the root path), so one profile shared across repositories never confuses two `README.md`. Re-inserting identical content attaches the new sources to the existing node. `graft sources diff [--root DIR]` re-hashes the recorded files and lists each as unchanged / changed / removed with the memories it supports; `graft sources refresh <id>` stores the new fingerprints after a memory was revalidated, without touching its content. `graft get` shows the sources, which survive export/import, `profile merge` and remote sync. Also accepted by `POST /v1/insert` and the MCP `graft_insert` tool. ([#4](https://github.com/AEndrix03/Graft/issues/4))
- `/learn docs [path]` ingests all the documentation of a repository (README, `docs/`, ADRs, guides, CONTRIBUTING, ARCHITECTURE) and nothing else: no issues, PRs, comments or web pages. Discovery is deterministic (tracked files only, skipping vendored, generated and submodule trees, licenses and agent instruction files), each document is distilled into retrieval-shaped nodes deduplicated against the graph, large doc sets run in bounded, resumable batches, and with `--source` provenance a re-run only processes new or changed documents ([#15](https://github.com/AEndrix03/Graft/issues/15)).
- `graft query --explain` lists every candidate the verifier scored, in vector order, with its rank, hit level and signals, to see why a question got the answer it did.
- **Plugin marketplace for Claude Code and Codex.** The repo is now a marketplace for both agents: `/plugin marketplace add AEndrix03/Graft` then `/plugin install graft@graft` on Claude Code, `codex plugin marketplace add AEndrix03/Graft` then `codex plugin add graft@graft` on Codex. The plugin carries the six skills (namespaced as `graft:<name>`) and updates through the marketplace.
- `/graft-init` installs the `graft` CLI when it is missing, after asking, by running the official installer; offers `graft upgrade` when the CLI is older than the plugin; and offers to remove skill copies left by an older `graft setup`, which would otherwise show every skill twice.
- `bench/`: reproducible benchmarks. `run.py` measures recall quality and latency on a private daemon with built-in defaults, over 65 notes and an independently written held-out question set (the author's dev set is kept apart and reported as optimistic). `agent.py` asks Claude Code the same questions with and without graft and grades the answers, reporting right and made-up answers, tokens and time. `charts.py` renders the README charts from the results.
- The installers write a minimal `config.yaml` (just the paths that depend on the install location) instead of copying the 403-line example. Built-in defaults cover the rest, so tuning improvements in later releases reach existing installs; `config.example.yaml` ships alongside as the documented reference.
- **One-line installers** at the repo root: `install.sh` (Linux/macOS) and `install.ps1` (Windows). They download the prebuilt release archive for the platform, verify it against the published `SHA256SUMS` and refuse to continue on a mismatch, extract into `~/.graft`, fetch the BGE-M3 model once, write `config.yaml` with absolute paths without overwriting an existing one, put `~/.graft/bin` on `PATH`, and run a smoke check. No compiler, no submodules, no MSYS2.
- `graft setup` with no argument now sets up every agent whose config directory exists on the machine, and the installers run it for you, so the only manual step left is `/graft-init` inside the agent.

### Changed

- The skills moved from `integrations/standard/skills` to `plugins/graft/skills`, the single source for the plugin, `graft setup` and the release archives (installed layout unchanged). `graft setup` skips Claude Code and Codex when the graft plugin is installed there.
- CI and the release `prep` job fail when the plugin manifests' `version` differs from `VERSION`.
- GitHub Release notes now contain only that version's `CHANGELOG.md` section instead of the whole file, and the release workflow refuses to start when the version has no dated changelog entry.
- `graft setup` installs skills and nothing else. All hook installation, `settings.json` / `hooks.json` / `config.toml` merging and instruction-file writing were removed from the binary (~290 lines, plus the shipped hook scripts). Agent wiring is done by `/graft-init` from inside the agent.
- `/graft-init` asks one question (global or project) instead of four, and writes the rule to `CLAUDE.md` + `.claude/rules/graft.md` on Claude Code, or `AGENTS.md` elsewhere.
- The `graft` skill was rewritten around the prompter model: a near hit is useful, a miss is a gap to fill, short search-engine-style queries, `classify` for keywords, verify before trusting a `STRONG`, supersede (`graft maintain resolve`) rather than delete a stale node, and a one-line end-of-turn recap.
- `scripts/install.sh` / `scripts/install.ps1` are now `scripts/build-from-source.sh` / `scripts/build-from-source.ps1` - they build from source and are for contributors, GPU builds and platforms without a prebuilt archive.

### Fixed

- On Windows a second daemon connection from the same process failed, and the CLI then auto-started a second `graftd` on the same socket: closing a connection called `WSACleanup` without resetting the "Winsock initialised" flag.
- Concurrent daemon requests could interleave their SQLite transactions: every worker shares one connection, and `SQLITE_OPEN_FULLMUTEX` serializes single calls, not a `BEGIN IMMEDIATE ... COMMIT` sequence, so parallel inserts, consolidate or sync failed with storage errors or committed another request's half-done work. Every storage call now holds a mutex on the connection for its whole duration. Two identical inserts racing past the content-hash check now both return the same node, the second with `duplicate: true`, instead of failing ([#7](https://github.com/AEndrix03/Graft/issues/7)).
- `graft profile merge --overwrite` could delete graph relationships that existed only in the target. When the target held the same content under a different node id, the merge's `INSERT OR REPLACE` deleted the target node, and with it every edge and keyword link pointing at it, then re-created the node under the source id. The merge now maps each source node onto the target node with the same content hash (or id), updates its metadata in place, inserts only nodes the target lacks, and imports the source's edges, keyword links and embeddings onto the retained ids ([#11](https://github.com/AEndrix03/Graft/issues/11)).
- Stopping the daemon while a request was in flight could crash it: client threads were detached and nothing waited for them, so shutdown freed the database, the models and the HTTP server while a request was still using them. The daemon and the HTTP server now count their client threads; shutdown stops accepting, shuts down the client sockets so an idle or half-sent connection cannot hold it up, and waits for the threads still working (up to 30 s per listener, after which it exits without freeing the state they use). SIGINT/SIGTERM no longer close the listening socket from the signal handler, which did not wake `accept()` on Linux ([#8](https://github.com/AEndrix03/Graft/issues/8)).
- Stopping the HTTP server hung forever on Linux, and with it daemon shutdown when `http.enabled` is on: closing the listening socket wakes a blocked `accept()` on Windows but not on Linux. The accept loop now waits in `select()` with a 200 ms timeout and checks the stop flag. Every ctest test also has a 300 s timeout, so a hang fails CI instead of holding the runner for 6 hours.
- `http.bind: localhost` and `http.bind: "::1"`, both documented as valid local binds, were rejected at startup: the server parsed the address with `inet_addr`, IPv4 numbers only. It now resolves it with `getaddrinfo` and listens on IPv4 or IPv6, preferring IPv4 when a name resolves to both; the loopback check runs on the resolved address ([#13](https://github.com/AEndrix03/Graft/issues/13)).
- `graft profile export` copied only `graft.db`, so after a daemon crash it silently dropped memories committed to `graft.db-wal` but not yet checkpointed. Export and import now use the SQLite backup API: the snapshot includes the WAL, and an import can no longer have a stale WAL replayed over it ([#10](https://github.com/AEndrix03/Graft/issues/10)).
- A keyword containing `,` is now rejected. The content hash joins keywords with `,`, so `["a,b", "c"]` and `["a", "b,c"]` hashed alike and the second insert came back as a false duplicate. Existing hashes are unchanged; the CLI says to pass each keyword as its own `--keyword` ([#12](https://github.com/AEndrix03/Graft/issues/12)).
- A `GRAFT_DB_KEY` with many single quotes overflowed a heap buffer when opening the database: each quote is doubled in `PRAGMA key`, but the buffer was sized for the key's own length. The statement is now built with `sqlite3_mprintf` ([#9](https://github.com/AEndrix03/Graft/issues/9)).
- The source-build scripts ended by suggesting `graft insert --summary ... --detail ...`, flags removed long ago: the request went out with an empty title and body and the daemon rejected it. They now print `--title` / `--body`, and the CLI refuses an unknown option or a known one missing its value (exit code 2) instead of silently ignoring it ([#14](https://github.com/AEndrix03/Graft/issues/14)).
- Building from source on Linux and macOS works again: `src/cli/autostart.c` called the POSIX `mg_spawn_daemon` with the Windows-only process-handle argument added by the daemon log fix, so the CLI failed to compile on every non-Windows platform.
- The MCP server pins `mcp<2`. A fresh `pip install -e .` pulled `mcp` 2.x, where `FastMCP` no longer exists, and the server refused to start with "mcp SDK not installed".
- The daemon's startup failure reason now reaches the user. `graftd` already printed the real cause on stderr ("embed init failed", "storage open failed", "socket listen failed"), but on Windows it was spawned DETACHED with no stdio redirection, so the output was discarded and the CLI only said "socket did not become ready in 20000 ms". The spawned daemon now inherits a handle to the log on both platforms, the log lives at `$GRAFT_HOME/graftd.log` as the docs always claimed (it used to sit next to the binary), and a failed auto-start quotes the tail of it.
- A daemon that exits during startup is detected immediately on Windows instead of after the full 20 s poll: a missing model now reports in about 1 second.
- `ctest` on Windows no longer fails with `0xc0000139` (STATUS_ENTRYPOINT_NOT_FOUND) for the six tests that import llama/ggml directly: CMake now prepends the llama.cpp build directories and the toolchain runtime directory to the test PATH. The CLI was unaffected because the linker drops its unused llama imports, which is what made the failure look like a code problem. 11/11 tests pass.

### Removed

- The harness hook scripts (`query_inject.js`, `mark_candidate.js`, `propose_memoryze.js`) are no longer installed by `graft setup`; they stay available as an opt-in in `integrations/optional/hooks/`, and the Claude Code plugin now ships its own prompt and session hooks. `scripts/install-codex-hooks.*` is gone.
- The duplicated per-agent skill copies under `integrations/claude-code/skills/`; `plugins/graft/skills/` is the single source.

## [0.1.1] - 2026-09-25

Patch release fixing the Windows daemon.

### Fixed

- The Windows release archive now ships `libgomp-1.dll`. `ggml-cpu.dll` imports the GCC OpenMP runtime, so `graftd.exe` exited immediately with `0xC0000135` (STATUS_DLL_NOT_FOUND) on a fresh install and every `graft` command failed with "socket did not become ready". The CLI was unaffected because it never loads the embedding backend.
- The Windows packaging step now walks the full PE import closure of the staged binaries instead of copying a hand-maintained DLL list, and fails the release if any import is neither bundled nor a Windows system DLL.

## [0.1.0] - 2026-05-16

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

[Unreleased]: https://github.com/AEndrix03/Graft/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/AEndrix03/Graft/compare/v0.1.1...v0.2.0
[0.1.1]: https://github.com/AEndrix03/Graft/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/AEndrix03/Graft/releases/tag/v0.1.0
