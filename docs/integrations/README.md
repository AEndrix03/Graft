# Integrations

Graft is "just a binary" by design — anything that can spawn a subprocess can use it. The `integrations/` directory ships the glue for the major AI development surfaces, so you do not have to write that glue yourself.

```
┌─────────────────────────────┐    ┌─────────────────────────────┐
│ LLM chat clients            │    │ Coding agents (CLI-based)   │
│ Claude Desktop · ChatGPT    │    │ Claude Code · Codex · ...   │
└──────────────┬──────────────┘    └──────────────┬──────────────┘
               │ MCP (stdio or HTTP)              │ subprocess
               ▼                                  ▼
┌─────────────────────────────┐    ┌─────────────────────────────┐
│ integrations/mcp-server/    │    │ graft CLI                   │
│  · server.py  (stdio)       │───▶│  → unix socket              │
│  · oauth_gateway.py (HTTP)  │    │                             │
└─────────────────────────────┘    └──────────────┬──────────────┘
                                                  ▼
                                     ┌─────────────────────────────┐
                                     │ graftd (daemon)             │
                                     │  SQLite + sqlite-vec + FTS5 │
                                     │  + BGE-M3 (llama.cpp)       │
                                     └─────────────────────────────┘
```

Two families:

| Family | Mechanism | Where it lives |
| ------ | --------- | -------------- |
| **CLI assistants** (Claude Code, Codex, Open Code, Gemini CLI) | The harness already spawns subprocesses. We ship skills / `AGENTS.md` / `GEMINI.md` files that instruct the model when to call `graft`, plus hooks that fire **deterministically** so the model can't "forget". | `integrations/claude-code/`, `integrations/codex/`, `integrations/opencode/`, `integrations/gemini-cli/` |
| **Chat clients** (Claude Desktop, ChatGPT) | No subprocess in the client. We expose graft as an **MCP server** (the [Model Context Protocol](https://modelcontextprotocol.io)). | `integrations/claude-ai/`, `integrations/chatgpt/`, both backed by `integrations/mcp-server/` |

## Matrix

| Agent          | Integration type             | Where it lives                                       |
| -------------- | ---------------------------- | ---------------------------------------------------- |
| Claude Code    | Skills + Hooks               | `integrations/claude-code/`                          |
| Codex          | `AGENTS.md` + Hooks          | `integrations/codex/`                                |
| Claude Desktop | MCP server (stdio)           | `integrations/claude-ai/` + `integrations/mcp-server/` |
| ChatGPT        | MCP server (stdio or HTTP)   | `integrations/chatgpt/` + `integrations/mcp-server/` |
| Gemini CLI     | `GEMINI.md` memory file      | `integrations/gemini-cli/`                           |
| Open Code      | `AGENTS.md` + Skills         | `integrations/opencode/`                             |

Each adapter has its own README with install steps.

---

## Two complementary layers

For the CLI assistants we ship **two** things that look similar but behave very differently:

### Skills (or `AGENTS.md`, or `GEMINI.md`)

These are **prompts**. They tell the model **when** to use graft:

- search before answering non-trivial questions,
- save after solving non-obvious ones,
- skip for trivial work (formatting, renaming).

The model decides whether to follow the instruction. Skills are useful — they shape what the agent **wants** to do.

For Claude Code we ship six skills:

| Skill            | Purpose |
| ---------------- | ------- |
| `graft`          | The master skill: orchestration, profile guidance, CLI reference, troubleshooting. |
| `graft-init`     | One-shot configurator: writes a `<!-- graft:start -->...<!-- graft:end -->` block into `CLAUDE.md`. |
| `recall`         | Smart search: tries `query`, falls back to `retrieve`, then to `explore`, escalating only when results are weak. |
| `memoryze`       | Distills the current conversation into 1–5 well-formed nodes and saves them. |
| `learn`          | Batch-ingestion from external sources (codebase, docs tree): plan + confirm + ingest. |
| `memory-audit`   | Read-only health check: hit rate, hoarding ratio, top reused nodes, never-reused nodes. |

All six are copied into `~/.claude/skills/` (or `.claude/skills/`) by `graft setup claudecode`.

### Hooks

These are **scripts** run by the harness deterministically. They do not rely on the model remembering to invoke them.

For Claude Code and Codex we ship three hooks under `hooks/graft/`:

| Hook                    | Fires on              | What it does |
| ----------------------- | --------------------- | ------------ |
| `query_inject.js`       | `UserPromptSubmit`    | Runs `graft query <prompt>`; if STRONG, injects the title + body into the agent's context. The agent sees the answer **before** it starts thinking. |
| `mark_candidate.js`     | `PostToolUse` (Edit / Write / Bash) | Records the edited content as a save-candidate. |
| `propose_memoryze.js`   | `Stop`                | At end-of-turn, proposes `/memoryze` if the conversation has accumulated unsaved learnings. |

Hooks are what take "the model usually does the right thing" to "the harness guarantees it". Skill + hook is the recommended setup; just-skill is acceptable; just-hook is wasteful (you save without context).

---

## MCP server (`integrations/mcp-server/`)

For chat clients that can't run subprocesses, MCP is the bridge.

### Local stdio (development)

```bash
cd integrations/mcp-server
pip install -e .             # or: uv pip install -e .

# Start graftd locally first.
graft stats

# Configure your client to launch:
python integrations/mcp-server/server.py
```

The stdio server wraps the `graft` CLI and speaks MCP on stdin / stdout. No auth — intended for local development.

### Production HTTP gateway (`oauth_gateway.py`)

An ASGI resource server. Mounts MCP streamable HTTP at `/mcp` and proxies authenticated REST calls under `/v1/*` to the local daemon (default `http://127.0.0.1:9977`).

```bash
export GRAFT_OAUTH_ISSUER_URL="https://issuer.example.com"
export GRAFT_OAUTH_RESOURCE_SERVER_URL="https://graft.example.com/mcp"
export GRAFT_OAUTH_AUDIENCE="https://graft.example.com"
export GRAFT_OAUTH_REQUIRED_SCOPES="graft:read"
export GRAFT_UPSTREAM_HTTP="http://127.0.0.1:9977"

uvicorn oauth_gateway:app --host 127.0.0.1 --port 8080
```

Put HTTPS termination in front (Caddy, nginx, Traefik). The external OIDC provider handles login, consent, client registration, and token issuance; graft validates access tokens, audience, issuer, expiration, and scopes.

### Tools exposed via MCP

Read tools (require `graft:read` in remote mode):

`graft_query`, `graft_retrieve`, `graft_explore`, `graft_classify`, `graft_get`, `graft_stats`, `graft_analytics`.

Write tools (require `graft:write`):

`graft_insert`.

Admin tools (require `graft:admin`):

`graft_delete`, plus the profile-management tools (`profile add/remove/import/export/merge/list/current`).

### Scope policy at the REST proxy

| Scope            | REST endpoints |
| ---------------- | -------------- |
| `graft:read`     | `GET /v1/match`, `/v1/search`, `/v1/explore`, `/v1/classify`, `/v1/nodes/{id}`, `/v1/view` |
| `graft:write`    | `POST /v1/insert` |
| `graft:admin`    | `DELETE /v1/nodes/{id}` |

`GET /v1/healthz` is always unauthenticated, for container probes.

---

## Operation-name mapping

For consistency, every adapter exposes the same set of operations under the same names:

| CLI subcommand     | MCP tool name        | What it does |
| ------------------ | -------------------- | ------------ |
| `insert`           | `graft_insert`       | Save a new node (title + body + keywords + optional supersession). |
| `query`            | `graft_query`        | Cache lookup (STRONG / WEAK / MISS) with multi-signal gating. |
| `retrieve`         | `graft_retrieve`     | Hybrid top-k via RRF. |
| `explore`          | `graft_explore`      | Beam-search graph walk. |
| `get`              | `graft_get`          | Fetch a node by `id_hex`. |
| `delete`           | `graft_delete`       | Hard-delete by id (admin only on remote). |
| `classify`         | `graft_classify`     | Suggest keywords for a draft title. |
| `stats`            | `graft_stats`        | Counts + similarity percentiles. |
| `analytics`        | `graft_analytics`    | Streams the local usage log. |
| `consolidate`      | _(not exposed)_      | CLI-only; admin pass. |

---

## Guidance for an LLM author

If you're writing your own skill / `AGENTS.md`, the rule of thumb is:

1. **Search before writing.** Always call `query` (or `retrieve`) **before** `insert`. If STRONG hit → reuse; if MISS → consider inserting.
2. **`query` vs `retrieve`.** `query` returns one verified result. `retrieve` returns up to N candidates. Use `query` when you want a confidence-gated answer; `retrieve` when you want a list to choose from.
3. **`explore` for related problems.** "What do I know about X?" or "What's connected to this decision?".
4. **`classify` before `insert`** if the user did not pass explicit keywords.

---

## Per-adapter notes

### Claude Code

`graft setup claudecode` copies skills + hooks into `~/.claude/skills/` and `~/.claude/hooks/`. Re-running is safe — it overwrites in place. Read [`../../integrations/claude-code/README.md`](../../integrations/claude-code/README.md) for the full setup, including the recommended `permissions.allow` entries to reduce permission prompts.

### Codex

`AGENTS.md` (the OpenAI Codex equivalent of `CLAUDE.md`) is shipped with graft instructions. Hooks are identical to Claude Code's — same Node scripts, different harness names.

### Claude Desktop / ChatGPT

Configure the MCP server in the client's connector JSON. Examples are shipped:

- `integrations/claude-ai/claude_desktop_config.json`
- `integrations/chatgpt/mcp_config.json`

Both wire the stdio MCP server (`integrations/mcp-server/server.py`) so the chat client can call the graft tools.

### Gemini CLI

`GEMINI.md` is shipped with the same "search before / save after" instructions as the other CLI assistants. No hook layer yet (the Gemini CLI's hook surface is younger).

### Open Code

`graft setup opencode` copies the shared `AGENTS.md` instructions and native skills into `~/.config/opencode/`. The Open Code harness reads `AGENTS.md` on launch and loads skills from `~/.config/opencode/skills/`.

---

## What's missing and how to improve it

- **JetBrains / VS Code extensions.** The CLI works fine when invoked from a build task, but a real extension that surfaces `graft query` on hover or `graft retrieve` from a code-action context would be a major UX win.
- **Aider / Continue / Cursor integrations.** Each has its own custom-tool surface; the graft CLI is small enough to wrap in any of them in under 100 lines, but the wrappers are not in this repo yet.
- **A common "agent integration test" target** that drives `query → insert → query` end-to-end and checks the STRONG-hit cycle. Today the integrations are tested manually.
- **Capability auto-detection on first run.** When an integration starts, it could call `graft stats` to check the daemon's health and `graft profile current` to print the active profile. Today the user has to know to do that.
- **A non-MCP HTTP path** for chat clients without MCP. The OAuth gateway is the obvious place, but exposing `graft_*` tools as plain HTTP endpoints (a custom GPT action, say) would broaden reach.
- **Multi-language SDK shims.** The CLI is the universal contract, but a thin TypeScript / Python wrapper would make web integrations cleaner.
