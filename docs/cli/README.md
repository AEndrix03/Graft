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
- [`sources`](#sources)     (provenance freshness: `diff`, `refresh`)
- [`project`](#project)     (bootstrap coverage state: `status`, `mark`, `reset`; CLI-only)
- [`maintain`](#maintain)   (autonomous maintenance: `status`, `scan`, `resolve`, `apply-safe`, `log`)
- [`analytics`](#analytics) (CLI-only — never touches the daemon)
- [`profile`](#profile)   (CLI-only)
- [`setup`](#setup)       (CLI-only)
- [`upgrade`](#upgrade)   (CLI-only)
- [`view`](#view)         (opens the browser viewer at the daemon's HTTP layer)

An option a command does not know, or a known option without its value, is an error (exit code `2`) before anything reaches the daemon.

---

## insert

```bash
graft insert \
  --title  "Short retrieval-shaped statement of what you learned" \
  --body   "Longer prose: the why, the trap, the workaround" \
  --keyword spring-boot --keyword validation --keyword gotcha \
  [--author "name@host"] \
  [--expires-at <unix-ms>] \
  [--source file:src/auth/JwtService.java] \
  [--tag <kw>]   # alias for --keyword
```

Idempotent on the content hash (`title + body + sorted keywords`). The first insert returns `duplicate: false`; an identical second insert returns `duplicate: true` and the existing `id_hex`.

`--source` (repeatable, at most 64) records where the memory comes from:

| Locator | Stored as |
|---|---|
| `file:<path>` | the path relative to its project root, `/`-separated, plus the project id and the BLAKE3 fingerprint of the file's bytes |
| `url:<url>` | the URL verbatim |
| `conversation[:tag]` | learned in a conversation |
| `manual[:tag]` | written by hand |

A `file:` path is resolved from the working directory by the CLI itself, which also hashes the file; a missing file is an error (exit `2`) before the daemon is contacted. The **project root** is the nearest ancestor holding a `.git` entry (a linked worktree's `.git` file is followed to its repository); without one, the working directory when it contains the file, else the file's own directory. The **project id** keeps one profile shared across many repositories collision-free (`README.md` in two repos are two sources): it is the repository's `origin` remote normalized to `host/path` (scheme, credentials and the trailing `.git` dropped, host lowercased, so the ssh and https clones of a repo agree, e.g. `github.com/AEndrix03/Graft`), or the absolute root path when there is no `origin`.

Inserting identical content again with new `--source` values attaches them to the existing node (`duplicate: true`, plus `sources_attached`), so repeated ingestion runs accumulate provenance instead of failing; re-attaching a file source records its current fingerprint. Nodes inserted without `--source` carry no provenance and are otherwise unaffected. See [`sources`](#sources) for checking freshness later.

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

`--explain` adds `candidates` to any of the three: every candidate the verifier
scored, in vector order, with `id_hex`, `title`, `vec_rank`, its own `hit` and
`signals`. Use it to see why a question got the answer it did.

Defaults:

| Threshold | Default | Tune in `config.yaml` |
| --------- | ------- | --------------------- |
| Cosine sanity floor | `0.30` | hard-coded |
| `STRONG` (lexical path) requires `s_vec >= …` and `s_lex >= …` | `0.70`, `0.15` | `verification.lex_strong_min_vec`, `cache.strong_hit_min_lex` |
| `STRONG` (semantic path) requires `s_vec >= …` | `0.75` | `verification.sem_strong_min_vec` |
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
sources:         # skipped if none
  - file:src/auth/JwtService.java @3f9a1c27b0de (changed)
  - conversation
---

<body>
```

The JSON form carries the same provenance as `sources: [{kind, locator, project, fingerprint, role, observed_at}]`, and the node's lifecycle `state` (`active`, `stale`, `superseded`, `retired`): `get` still answers for a retired node, which no search returns. In the Markdown form a file source shows the first 12 hex digits of the fingerprint recorded for this node and, when it belongs to the working directory's project, its state (`unchanged`, `changed`, `removed`); a file source of another project shows that project instead.

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
- delete source rows no node links to any more (`maintenance.orphan_sources_deleted`; deleting a node already drops its links),
- report graph-health signals (isolated nodes, bidirectional pairs, contradictions found).

The pass is **non-destructive** for semantic content — it never merges nodes by similarity. Semantic cleanup goes through [`maintain`](#maintain): Graft reports candidates, the agent decides, one narrow action applies the decision. `maintain apply-safe` runs this same pass.

---

## sources

```bash
graft sources diff [--root <dir>] [--changed-only]
graft sources refresh <hex_id> [--root <dir>]
```

Provenance freshness for the `file:` sources recorded with [`insert --source`](#insert). Both commands resolve the project of `--root` (default: the working directory) exactly like `insert` does, so they can run from any subdirectory of the repository. The daemon only reads the recorded rows (no embedding); the files are re-hashed by the CLI, and nothing but their BLAKE3 fingerprint is ever computed from them.

`diff` re-hashes every file source recorded for the project and classifies it:

| State | Meaning |
|---|---|
| `unchanged` | the file still matches the fingerprint recorded on every node it supports |
| `changed` | it no longer matches the fingerprint of at least one node |
| `removed` | the file is gone |
| `unavailable` | it exists but cannot be read |

```json
{
  "status": 0,
  "result": {
    "project": "github.com/AEndrix03/Graft",
    "root": "/home/me/src/graft",
    "summary": { "sources": 12, "unchanged": 10, "changed": 1, "removed": 1,
                 "unavailable": 0, "nodes_to_revalidate": 2 },
    "sources": [
      { "kind": "file", "locator": "src/auth/JwtService.java", "state": "changed",
        "fingerprint": "<current blake3 hex>",
        "nodes": [ { "id_hex": "019e...", "title": "...", "state": "changed",
                     "recorded_fingerprint": "<blake3 hex>" } ] }
    ]
  }
}
```

Each node carries its own state, because each link records the version that node was derived from (or last revalidated against). `--changed-only` leaves `unchanged` sources out of the list (the summary still counts them). Superseded nodes are not listed. A changed source never deletes anything: the agent compares the memory with the file and keeps it, supersedes it with a corrected node, or deletes it.

`refresh` is the "keep it" step: after revalidating a node, it re-hashes the node's file sources of this project and stores the new fingerprints (and `observed_at`) on its links, without touching the node's content. A removed file is reported and left as is; a source of another project is reported as `skipped`.

```json
{ "status": 0, "result": { "id_hex": "019e...", "project": "...", "updated": 1,
  "sources": [ { "locator": "src/auth/JwtService.java", "state": "changed", "refreshed": true,
                 "previous_fingerprint": "...", "fingerprint": "..." } ] } }
```

`state` is what was found before the refresh. Exit codes: `2` for usage errors or a `--root` that is not a directory, `3` when the daemon reports an error (e.g. unknown node id).

---

## project

```bash
graft project status [--root <dir>]
graft project mark   [--root <dir>] [--topic <name>]... [--state covered|pending|skipped|drop]
                     [--priority high|normal|low] [--note <text>] [--run]
graft project reset  [--root <dir>]
```

The bootstrap coverage state of a project in the active profile: what [`/learn bootstrap`](../integrations/README.md) has covered and what is left, so a later run (or `/graft-init` at session start) knows without rescanning the repository. CLI-only: it never starts or calls the daemon, never loads the model. The project is resolved from `--root` (default: the working directory) exactly like [`insert --source`](#insert) does.

`status` reports:

```json
{
  "status": 0,
  "result": {
    "project": "github.com/AEndrix03/Graft",
    "root": "/home/me/src/graft",
    "profile": "default",
    "state_file": "/home/me/.graft/profiles/default/projects/c2c197f1c59356277884ff481217e62d.tsv",
    "bootstrapped": true,
    "runs": 2,
    "last_run_at": 1791147390000,
    "provenance": { "files": 31, "nodes": 44 },
    "topics": { "covered": 7, "pending": 3, "skipped": 1 },
    "pending": [
      { "topic": "http-viewer", "priority": "normal", "note": "src/http/, viewer/", "updated_at": 1791147390000 }
    ],
    "covered": [ "overview", "storage", "..." ],
    "skipped": [ "vendored-deps" ]
  }
}
```

| Field | Meaning |
|---|---|
| `bootstrapped` | at least one bootstrap run was marked (`mark --run`); it does not mean every topic is covered |
| `last_run_at` | unix ms of the last marked run, `null` if none |
| `provenance` | file sources of this project backing at least one live node, and those nodes, read from the profile DB (read-only; safe beside a running daemon). `{files: 0, nodes: 0}` when the DB does not exist yet; `null` plus a `provenance_error` when it cannot be read, e.g. a DB not yet opened by a graft with provenance |
| `pending` | pending topics, high priority first, then in the order they were recorded |

`mark` upserts topics (`--topic` is repeatable; every topic named gets the same `--state`, `--priority` and `--note`; an omitted priority or note keeps the stored one, `--state drop` removes the topic) and, with `--run`, records a completed bootstrap run (`runs + 1`, `last_run_at = now`). It prints the same report as `status`. Tabs and newlines in names and notes become spaces; names are capped at 200 bytes, notes at 500.

`reset` deletes the project's state (`{project, state_file, removed}`); provenance and nodes are untouched.

The state lives next to the DB it describes, in `<db dir>/projects/<hash of the project id>.tsv` (one per profile, or per `GRAFT_DB_PATH`): a small line-oriented file written atomically, not meant to be edited by hand. Exit codes: `2` for usage errors or a `--root` that is not a directory, `1` when the state file cannot be read or written.

---

## maintain

```bash
graft maintain status
graft maintain scan [--limit N] [--root <dir>] [--no-sources]
graft maintain resolve [<candidate-id>] --action <action> [--node <id>] [--by <id>] [--note <text>] [--actor <name>]
graft maintain apply-safe
graft maintain log [--limit N] [--node <id>]
```

The maintenance protocol: Graft does the mechanical part itself (`apply-safe`) and reports what needs judgment as **candidates** (`scan`); the integrated agent checks each candidate against the current code / docs and applies its decision with one narrow, audited, reversible action (`resolve`). The protocol is described in [`maintenance/`](../maintenance/#autonomous-maintenance-graft-maintain).

### status

Cheap (indexed counts and the candidate cache, no embedding): what an agent checks before deciding whether maintenance is due.

```json
{ "last_apply_safe_at": 1791148203000, "last_scan_at": 1791148203000,
  "inserts_since_apply_safe": 12, "inserts_since_scan": 3,
  "pending": { "total": 2, "by_kind": { "contradiction": 0, "source_removed": 0, "source_changed": 1,
               "possible_supersession": 0, "near_duplicate": 1,
               "keyword_fragmentation": 0, "isolated_low_value": 0 } },
  "nodes": { "active": 120, "stale": 3, "superseded": 9, "retired": 1, "retired_due": 0 },
  "retention_days": 30, "trigger_inserts": 50,
  "recommended": [ "resolve" ] }
```

`recommended` lists what is due: `apply-safe` (never run, `trigger_inserts` inserts since the last run, or retired nodes past retention), `scan` (never run on a graph with more than one active node, or `trigger_inserts` inserts since the last scan) and `resolve` (pending candidates). Empty means nothing to do.

### scan

Emits candidates, most urgent first, **without changing any node, edge or source**; it only replaces the candidate cache that `status` counts and `resolve` reads. Like `sources diff`, it re-hashes on the CLI side the `file:` sources recorded for the project of `--root` (default: the working directory); `--no-sources` skips that (no provenance candidates). Nodes without file sources are simply not provenance candidates.

```json
{ "summary": { "pending": 2, "by_kind": { "...": 0 }, "dismissed": 1, "truncated": false,
               "scanned_nodes": 120, "sources_checked": true },
  "candidates": [
    { "id": "14c40c58e7bab133", "kind": "possible_supersession", "score": 0.9923,
      "nodes": [ { "id_hex": "019a...", "title": "Auth uses sessions", "state": "active",
                   "created_at": 1791140000000, "access_count": 4 },
                 { "id_hex": "019b...", "title": "Auth uses JWT", "state": "active",
                   "created_at": 1791148203000, "access_count": 0 } ],
      "signals": { "semantic_similarity": 0.9923, "shared_keywords": [ "auth" ],
                   "newer": "b", "age_gap_hours": 2, "source_changed": false },
      "suggested_actions": [ "supersede_a", "supersede_b", "keep_both", "merge" ] } ] }
```

| Kind (priority order) | Found when | Signals | Suggested actions |
|---|---|---|---|
| `contradiction` | a `CONTRADICTS` edge joins two active nodes | `edge_weight`, `shared_keywords` | `supersede_a`, `supersede_b`, `stale`, `keep_both` |
| `source_removed` | a file source of an active node is gone | `project`, `sources[]` | `retire`, `stale`, `supersede`, `keep` |
| `source_changed` | a file source of an active node no longer matches the fingerprint recorded on its link | `project`, `sources[]` with recorded and current fingerprint | `refresh`, `stale`, `supersede`, `retire` |
| `possible_supersession` | a near-duplicate pair sharing a keyword, created at least an hour apart | as `near_duplicate` | `supersede_a`, `supersede_b`, `keep_both`, `merge` |
| `near_duplicate` | two active nodes with cosine >= `maintenance.near_duplicate_min` | `semantic_similarity`, `shared_keywords`, `newer`, `age_gap_hours`, `source_changed` | `merge`, `supersede_a`, `supersede_b`, `keep_both` |
| `keyword_fragmentation` | a keyword carried by one active node while a spelling variant (case, `-` / `_`, plural) is in use | `keyword`, `similar_keyword`, uses of both | `keep`, `supersede` |
| `isolated_low_value` | an active node with no edge, never read, older than `maintenance.isolated_min_age_days` | `age_days` | `keep`, `retire`, `stale` |

Pair candidates list the **older node first** (`a`), so `supersede_a` ("a is replaced by b") is the usual outcome of a supersession. Candidate ids are deterministic (kind + node ids): the same finding keeps its id across scans. The work is bounded: near-duplicates compare the newest `maintenance.scan_max_nodes` active nodes pairwise and keep at most `maintenance.scan_neighbors` partners per node; at most `maintenance.scan_cap` candidates are kept and `--limit` (default 20) of them returned.

### resolve

Applies one decision, in one transaction with its audit row. A candidate id targets its nodes; `--node` acts on any node directly (next to a candidate id it must be one of the candidate's nodes).

| Action | Effect |
|---|---|
| `keep`, `keep_both` | dismiss the candidate: it stays hidden until its evidence changes (node content, current source fingerprint) |
| `stale` | `ACTIVE -> STALE`: still searchable, flagged as doubtful |
| `retire` | soft delete: hidden from `query` / `retrieve` / `explore`, restorable, physically deleted by `apply-safe` after `maintenance.retention_days` |
| `restore` | `STALE` / `SUPERSEDED` / `RETIRED -> ACTIVE`, dropping the `SUPERSEDES` edges that point at the node |
| `supersede --by <id>` | `SUPERSEDED` by `<id>`, with a `SUPERSEDES` edge. On a pair candidate, `--by` one of the two supersedes the other |
| `supersede_a`, `supersede_b` | pair candidates: `a` superseded by `b`, or `b` by `a` |
| `merge --by <id>` | after inserting the merged note `<id>`: every candidate node is superseded by it |
| `refresh` | `source_changed` only: store the fingerprints the scan observed on the node's links (the memory was revalidated) |

Without `--node`, `stale` / `retire` / `restore` / `supersede` need a single-node candidate. The result lists the touched nodes with their new state. Saving again the exact content of a retired node (`insert` deduplicates on content) restores it.

### apply-safe

The mechanical pass, safe to run any time:

1. purge retired nodes whose retention window has elapsed (one `purge` audit row each);
2. collapse exact duplicates: active / stale nodes with byte-identical title and body (they differ only in keywords). The oldest is kept, the others are superseded by it and their provenance links copied to it; `restore` undoes it;
3. the [`consolidate`](#consolidate) pass (expired nodes, orphan / duplicate / invalid rows, orphan sources, `ANALYZE`);
4. drop cached candidates whose nodes are no longer active;
5. record the run (`last_apply_safe_at`; `inserts_since_apply_safe` starts again from 0).

```json
{ "previous_apply_safe_at": 0, "last_apply_safe_at": 1791148203000, "inserts_since_last": 2,
  "retention_days": 30, "purged_retired": 0, "collapsed_duplicates": 0, "candidates_dropped": 0,
  "consolidate": { "expired_deleted": 0, "duplicate_edges_deleted": 0, "orphan_edges_deleted": 0,
                   "orphan_node_keywords_deleted": 0, "invalid_edges_deleted": 0,
                   "orphan_sources_deleted": 0, "sqlite_analyzed": true },
  "graph": { "n_nodes": 2, "n_edges": 2, "n_keywords": 1 } }
```

### log

The audit trail, newest first: every resolution, purge, collapse and apply-safe run. `--node` keeps the rows naming that node; `--limit` defaults to 50.

```json
{ "entries": [ { "id": 1, "ts": 1791148203000, "actor": "agent", "action": "keep",
                 "candidate_id": "14c40c58e7bab133", "kind": "near_duplicate",
                 "nodes": [ "019a...", "019b..." ],
                 "detail": "dismissed until the evidence changes", "note": "both are fine" } ] }
```

`actor` is `--actor`, else `GRAFT_AUTHOR`, else `agent`. Exit codes: `2` for usage errors or a `--root` that is not a directory, `3` when the daemon refuses (unknown candidate, a transition the node's state does not allow, a missing `--by`).

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
graft setup                 # every agent found on this machine
graft setup claudecode      # or name one explicitly
graft setup codex
graft setup opencode
```

Copies the shared skills package (`plugins/graft/skills/` in a checkout,
`share/graft/integrations/standard/skills/` in an install) into the agent's
user config directory (`~/.claude/skills`, `~/.codex/skills`, `~/.config/opencode/skills`).
With no argument it sets up every agent whose config directory exists. Claude Code
and Codex are skipped when the graft plugin is installed from the marketplace:
the skills already come from it, and a copy would list each one twice. Re-running
overwrites in place - safe and idempotent.

That is all it does. It installs **no hooks**, and it does not touch `settings.json`,
`config.toml`, `CLAUDE.md` or `AGENTS.md`. The instruction wiring is done from inside
the agent by `/graft-init`, which is one of the skills this command installs.

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
| `GRAFT_AUTHOR`  | `<user>@<host>` | Default author on `insert` (empty string opts out), and the audit `actor` of `maintain resolve` (default `agent`). |
| `GRAFT_USAGE_LOG` | `$GRAFT_HOME/usage.jsonl` | Override the usage log path. |

The full list lives in [`configuration/`](../configuration/).

---

## What's missing and how to improve it

- **`graft import` from text** (raw Markdown, NDJSON of `{title, body, keywords}`). The current entry point is N×`graft insert`, which is wasteful when ingesting whole folders. The `/learn` skill works around it on the agent side, but a native batch endpoint would be much faster.
- **`graft logs`** — print the last N lines from `~/.graft/memgraphd.err.log` with a colourised hit-level view. Today the user has to know where the log lives.
- **`graft doctor`** — single command that runs `stats`, prints the percentiles, checks model presence, checks socket, prints the resolved config, and lists active profiles. Useful for first-time setup.
- **Shell completion**. Bash / zsh / fish / PowerShell completion files are not generated. A `graft completions <shell>` subcommand printing the completion script would be a clean fix.
- **`--json` flag** that switches the pretty-printer to strict JSON output. Today the pretty-print is JSON-shaped but uses `'` for keys and trailing whitespace — fine for humans, less ergonomic for scripts that parse it.
