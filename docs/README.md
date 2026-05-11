# graft — documentation

This is the developer reference for **graft**, the persistent graph-memory daemon for AI agents. Each section is a feature folder; open the one you care about.

If you only want to **try graft**, jump to [`install/`](./install/) and you'll be running in under a minute.

If you want to **embed graft in your own service stack**, read [`microservices/`](./microservices/) first — that's the canonical L1 / L2 / L3 caching pattern.

If you want to **understand the internals**, start with [`architecture/`](./architecture/) and follow the diagram.

---

## Map

| Folder | What's inside |
| ------ | ------------- |
| [`install/`](./install/)               | Homebrew, install scripts, manual build, GPU builds, first-run check. |
| [`architecture/`](./architecture/)     | CLI ↔ daemon split, the wire protocol, the request/response lifecycle. |
| [`cli/`](./cli/)                       | Every `graft` / `graftd` subcommand and flag, with examples. |
| [`storage/`](./storage/)               | SQLite schema, `sqlite-vec`, FTS5, atomic supersession, idempotency, WAL. |
| [`embeddings/`](./embeddings/)         | BGE-M3 (1024-dim), llama.cpp, threading, CPU vs CUDA vs ROCm. |
| [`retrieval/`](./retrieval/)           | `query` (cache lookup), `retrieve` (RRF), `explore` (beam + MMR), the verify pipeline. |
| [`insert/`](./insert/)                 | Insert pipeline, keyword / semantic edge construction, MMR diversity, content hashing, `classify`. |
| [`profiles/`](./profiles/)             | Multi-tenancy, per-profile DB + socket + daemon, export / import / merge / remote sync. |
| [`http-api/`](./http-api/)             | Optional REST layer (`/v1/*`), per-endpoint flags, response envelope, examples. |
| [`viewer/`](./viewer/)                 | Browser 3D viewer (Vue + three.js + CodeMirror), build, modes, edit-with-supersession. |
| [`integrations/`](./integrations/)     | Claude Code, Codex, Claude Desktop, ChatGPT, Gemini CLI, Open Code, MCP / OAuth gateway. |
| [`microservices/`](./microservices/)   | The L1 Redis + L2 graft semantic + L3 graft + AI agentic stack. Read this before embedding graft. |
| [`maintenance/`](./maintenance/)       | `stats`, `consolidate`, the usage log, `analytics`, what to run when. |
| [`configuration/`](./configuration/)   | Every key in `config.yaml`, every recognised environment variable, defaults table. |

## Legacy single-file references

| File | Status |
| ---- | ------ |
| [`HTTP-API.md`](./HTTP-API.md) | Superseded by [`http-api/`](./http-api/). Kept for backward-compatible links. |
| [`Homebrew.md`](./Homebrew.md) | Superseded by [`install/`](./install/). Kept for backward-compatible links. |

## Conventions used in this documentation

- **Inline checkboxes** show stable / experimental / planned state.
  - ✅ stable — covered by tests, used by integrations, safe to depend on.
  - 🟡 experimental — works end-to-end but the API or the threshold defaults may change.
  - 🟠 planned — wiring is in place, but the implementation is a stub. See the "What's missing" section of the relevant page.
- Code samples assume `graft` is in `PATH`. The daemon (`graftd`) auto-starts on the first CLI call.
- Where examples show JSON, that's the same JSON-ish format the CLI prints (a deterministic pretty-print of the MessagePack response).

## How to contribute to the docs

Each page ends with a **"What's missing and how to improve it"** section that lists known gaps and TODOs. Pick one. Open a PR with the change plus a short note in `CONTRIBUTING.md` if the change is non-trivial. PRs are very welcome — graft is alpha and the docs are the friendliest place to start.
