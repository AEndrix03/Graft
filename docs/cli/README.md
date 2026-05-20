# CLI reference

Every subcommand `graft` accepts, in one place.

The binary `graft` is a thin client. It auto-spawns `graftd` on the first call of a session (the cold start pays the embedding-model load cost once). Every command prints a deterministic JSON-ish view of the daemon's MessagePack response.

```text
graft <subcommand> [args] [flags]
```

Exit codes:

| Code | Meaning |
| ---- | ------- |
| `0`  | OK. |
| `1`  | I/O error (socket, encode/decode, daemon not reachable). |
| `2`  | Usage error (bad argv, missing required flag). |
| `3`  | Daemon returned a non-zero `status`. The full error string is in the JSON output. |

---

## Index

- [`insert`](#insert)
- [`query`](#query)
- [`retrieve`](#retrieve)
- [`explore`](#explore)
- [`get`](#get)
- [`delete`](#delete)
- [`classify`](#classify)
- [`stats`](#stats)
- [`consolidate`](#consolidate)
- [`analytics`](#analytics) (CLI-only — never touches the daemon)
- [`profile`](#profile)   (CLI-only)
- [`setup`](#setup)       (CLI-only)
- [`upgrade`](#upgrade)   (CLI-only)
- [`view`](#view)         (opens the browser viewer at the daemon's HTTP layer)

---

## insert

```bash
graft insert \
  --title  "Short retrieval-shaped statement of what you learned" \
  --body   "Longer prose: the why, the trap, the workaround" \
  --keyword spring-boot --keyword validation --keyword gotcha \
  [--author "name@host"] \
  [--expires-at <unix-ms>] \
  [--tag <kw>]   # alias for --keyword
```

Idempotent on the content hash (`title + body + sorted keywords`). The first insert returns `duplicate: false`; an identical second insert returns `duplicate: true` and the existing `id_hex`.

Response shape:

```json
{
  "status": 0,
  "result": {
    "id_hex":      "019e0a4466...",
    "duplicate":   false,
    "n_kw_edges":  3,
    "n_sem_edges": 2
  }
}
```

`--author` defaults to `<user>@<host>` taken from the OS. Override with `GRAFT_AUTHOR=...`, or set it to empty (`GRAFT_AUTHOR=""`) to opt out entirely.

---

## query

```bash
graft query "the question or topic"
```

Verified **cache lookup**. Embeds the input, runs `vector_topk(10)`, then for each candidate computes trigram Jaccard + (optional) cross-encoder, picks the best by composite rank, and gates into `STRONG` / `WEAK` / `MISS`.

- **STRONG** — returns `id_hex`, `title`, `body`, and `signals`.
- **WEAK**   — returns `title` and `signals`. The `body` is intentionally `null`. (Don't quote a fact you can't verify.)
- **MISS**   — returns `signals` and a small `fallback_retrieve` list (capped at `retrieval.query_fallback_top_k`, default 5) so the caller can still surface neighbours.

Defaults:

| Threshold | Default | Tune in `config.yaml` |
| --------- | ------- | --------------------- |
| Cosine sanity floor | `0.30` | hard-coded |
| `STRONG` requires `s_vec >= 0.7` | `0.70` | hard-coded |
| `STRONG` requires `s_lex >= …`   | `0.15` | `cache.strong_hit_min_lex` |
| `WEAK` requires `s_vec >= …`     | `0.85` | `cache.weak_hit_min_vec` |
| `WEAK` requires `s_lex >= …`     | `0.05` | `cache.min_lex_overlap` |

> Read [`retrieval/`](../retrieval/) for the full multi-signal gating story.

---

## retrieve

```bash
graft retrieve "topic" [--top-k N]
```

Hybrid top-k. Returns nodes ranked by **Reciprocal Rank Fusion** over three independent lists:

- `R_vec` — vector top-k on the title embedding (cosine).
- `R_bm25_title` — FTS5 BM25 over `nodes.title`.
- `R_bm25_body`  — FTS5 BM25 over `nodes.body`.

Score is `Σ 1 / (k_const + rank_i)`. Default `k_const = 60`. Theoretical max for a node that ranks #1 in all three lists is `3 / 61 ≈ 0.0492`.

`--top-k` defaults to `retrieval.top_k` from config (25). Capped at 256 internally.

---

## explore

```bash
graft explore "topic" \
  [--keyword K]... \
  [--depth N] \
  [--beam N]
```

Keyword-conditioned **beam search** over the memory graph.

- Seed: `vector_topk(2 * beam)` filtered by the provided keywords (or unfiltered if none).
- Step: expand each beam through stored edges (KEYWORD + SEMANTIC), scoring with `log(edge_weight) + α·log(sem_score) − γ^step · depth_penalty`.
- Selection: greedy MMR (`mmr_lambda`) until `beam` candidates are kept.
- Output: visited nodes (with the best score and depth they were reached at) and the edges actually traversed.

Defaults from config (`explore.*`):

| Key | Default |
| --- | ------- |
| `depth`         | 3 |
| `beam`          | 4 |
| `decay_gamma`   | 0.85 |
| `alpha`         | 0.5 |

---

## get

```bash
graft get <hex_id>            # JSON-ish
graft get <hex_id> --markdown # human-readable YAML-frontmatter Markdown
```

Fetches a single node. The Markdown form prints the node as:

```markdown
---
title: ...
author: ...      # skipped if missing
date: 2026-05-12T13:00:00Z
expire on: ...   # skipped if 0
keywords: #spring-boot #validation
---

<body>
```

Optional rows are omitted when their underlying field is absent. Designed for human consumption; agents continue to use the JSON form.

---

## delete

```bash
graft delete <hex_id>
```

Hard-delete. Cascades to `node_keywords`, the FTS5 mirror, `node_vec` (sqlite-vec), and to every `edges` row referencing the node. Returns the deleted id or `MG_ERR_NOT_FOUND`.

> The HTTP equivalent is **off by default** (`endpoint_delete: false`). The CLI is the trusted local surface.

---

## classify

```bash
graft classify --title "your draft title"
```

Suggests 3–6 keywords drawn from the existing graph. Internally: `vector_topk(50)` on the title embedding, walk each result's KEYWORD edges, count keyword occurrences, sort by frequency. Result map:

```json
{ "status": 0, "result": { "suggested_keywords": ["spring-boot", "validation", "gotcha"] } }
```

If the graph is empty you get an empty list — there is no synthetic keyword model.

---

## stats

```bash
graft stats
```

Runtime metrics + similarity-distribution percentiles:

```json
{
  "status": 0,
  "result": {
    "n_nodes":    1284,
    "n_edges":    5612,
    "n_keywords": 312,
    "distributions": {
      "insert_topk": { "p25": ..., "p50": ..., "p75": ..., "p90": ..., "p95": ..., "p99": ... },
      "query_top1":  { "p25": ..., "p50": ..., "p75": ..., "p90": ..., "p95": ..., "p99": ... }
    }
  }
}
```

Use the percentiles to set thresholds. If your `query_top1.p50` is `0.78` and you're complaining that too many queries MISS, the right answer is to lower `cache.weak_hit_min_vec` toward your p50, not to scale a Python service.

---

## consolidate

```bash
graft consolidate
```

Safe maintenance pass:

- prune expired nodes,
- remove legacy orphan / duplicate / invalid edges,
- refresh SQLite planner stats (`ANALYZE`),
- report graph-health signals (isolated nodes, bidirectional pairs, contradictions found).

The pass is **non-destructive** for semantic content — it never merges nodes by similarity. Manual content consolidation is on the roadmap; for now you do it via [`/memoryze`](../integrations/) or by editing in the viewer.

---

## analytics

```bash
graft analytics                       # last 7 days
graft analytics --since 7d
graft analytics --since 24h
graft analytics --since 30d --seconds-per-hit 90
```

CLI-only. Streams `~/.graft/usage.jsonl` and prints aggregates:

- counts per op,
- average latency per op,
- cache hit rate (STRONG / WEAK / MISS) on `query`,
- estimated time saved at `<seconds-per-hit>` per STRONG hit.

The seconds-per-hit knob is a coarse calibration — pick a value that reflects how long the question would have taken a fresh agent to figure out from scratch. The aggregator never makes a network call and never touches the daemon.

---

## profile

```bash
graft profile list
graft profile current
graft profile add    <name>
graft profile remove <name> [--yes]
graft profile set    <name> [--shell bash|zsh|fish|powershell|cmd]
graft profile export <name> --path <file>
graft profile import --name <name> --file <file> [--force]
graft profile merge  --into <name> --from <file> [--overwrite]
graft profile remote bind   <name> --url <file-or-url> [--token T]
graft profile remote status <name>
graft profile remote sync   <name>
graft profile remote detach <name>
```

Each profile is a full tenant: its own DB, its own daemon, its own socket. `default` is created on first run.

`profile set` does **not** mutate your shell — it prints the right `export` / `$env:` / `set` line for the detected shell. You decide whether to `eval` it for the current session or persist it in your rc file.

See [`profiles/`](../profiles/) for the full multi-tenancy story.

---

## setup

```bash
graft setup claudecode
graft setup codex
graft setup opencode
```

Copies the shared skills package from `integrations/standard/skills/` into the agent's user config directory. Re-running overwrites in place - safe and idempotent.

`graft setup` does not install hooks and does not modify agent settings or `AGENTS.md`. Hook and project-instruction wiring remains manual in the per-agent integration docs.

---

## upgrade

```bash
graft upgrade
graft upgrade --check
graft upgrade --yes
```

Checks the latest GitHub Release, compares it with `graft --version`, prompts
for confirmation, downloads the platform archive plus `SHA256SUMS`, verifies
the archive hash, and updates the installed graft runtime.

`upgrade` only works from the standard install layout (`<root>/bin/graft`) and
does not overwrite user profiles, DBs, models, or `~/.graft/config.yaml`.

Environment overrides for forks/tests:

| Variable | Effect |
| -------- | ------ |
| `GRAFT_UPGRADE_REPO` | Override `owner/repo` used for GitHub Releases. |
| `GRAFT_UPGRADE_LATEST_URL` | Override the latest-release API URL. |

---

## view

```bash
graft view              # opens http://127.0.0.1:9977/
graft view --port 9977
```

Opens the **3D viewer** in your default browser. On first run the CLI **auto-builds** the viewer SPA (`npm install && npm run build` in `viewer/`) and points the daemon's `http.viewer_path` at the resulting `dist/`. Subsequent calls are instant.

Requires `http.enabled: true` in `config.yaml`. If it's off, the CLI prints the exact config snippet you need to add.

---

## Environment variables

Read by the CLI:

| Variable | Default | Effect |
| -------- | ------- | ------ |
| `GRAFT_SOCKET` | per-profile socket path | Override the daemon socket. |
| `GRAFT_DB_PATH` | per-profile DB path | Override the DB path the daemon should open. |
| `GRAFT_PROFILE` | `default` | Active profile (the CLI computes socket / DB paths from it). |
| `GRAFT_HOME`    | `~/.graft` | Where profiles, sockets, usage log live. |
| `GRAFT_CONFIG`  | _(auto-discovered)_ | Override the path to `config.yaml`. |
| `GRAFT_AUTHOR`  | `<user>@<host>` | Default author on `insert`. Empty string opts out. |
| `GRAFT_USAGE_LOG` | `$GRAFT_HOME/usage.jsonl` | Override the usage log path. |

The full list lives in [`configuration/`](../configuration/).

---

## What's missing and how to improve it

- **`graft import` from text** (raw Markdown, NDJSON of `{title, body, keywords}`). The current entry point is N×`graft insert`, which is wasteful when ingesting whole folders. The `/learn` skill works around it on the agent side, but a native batch endpoint would be much faster.
- **`graft logs`** — print the last N lines from `~/.graft/memgraphd.err.log` with a colourised hit-level view. Today the user has to know where the log lives.
- **`graft doctor`** — single command that runs `stats`, prints the percentiles, checks model presence, checks socket, prints the resolved config, and lists active profiles. Useful for first-time setup.
- **Shell completion**. Bash / zsh / fish / PowerShell completion files are not generated. A `graft completions <shell>` subcommand printing the completion script would be a clean fix.
- **`--json` flag** that switches the pretty-printer to strict JSON output. Today the pretty-print is JSON-shaped but uses `'` for keys and trailing whitespace — fine for humans, less ergonomic for scripts that parse it.
