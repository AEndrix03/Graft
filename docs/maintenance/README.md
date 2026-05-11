# Maintenance, observability, and the usage log

Graft's design assumption is that **graphs grow over time and need light, periodic maintenance**, the same way a notebook needs an occasional re-read. This page covers the tools that ship for it: `stats`, `consolidate`, the usage log, and `analytics`.

## At a glance

| Command | Touches the DB? | Touches the daemon? | When to run |
| ------- | ---------------- | ------------------- | ----------- |
| `graft stats`        | Read-only | Yes | Anytime you want a percentile report or a node-count check. |
| `graft consolidate`  | Read + lightweight write (prune, dedup, `ANALYZE`) | Yes | Periodically — every ~100 inserts, or before a long-running session. |
| `graft analytics`    | No (just reads `usage.jsonl`) | No | Anytime you want a hit-rate / latency / time-saved report. |
| Edit threshold knobs in `config.yaml` | No | Restart daemon | After you've looked at `stats` and decided to retune. |

The skills `/memory-audit` (Claude Code, etc.) call `stats` + `consolidate` + `analytics` and present the combined view.

---

## `graft stats`

Two numbers and one distribution:

```json
{
  "n_nodes":    1284,
  "n_edges":    5612,
  "n_keywords": 312,
  "distributions": {
    "insert_topk": { "p25": 0.41, "p50": 0.62, "p75": 0.78, "p90": 0.86, "p95": 0.91, "p99": 0.96 },
    "query_top1":  { "p25": 0.55, "p50": 0.71, "p75": 0.83, "p90": 0.89, "p95": 0.92, "p99": 0.96 }
  }
}
```

Where the percentiles come from:

| Sample kind | When recorded | Meaning |
| ----------- | ------------- | ------- |
| `insert_topk` (kind = 0) | One row per edge candidate at insert time. | Distribution of cosine similarities for edges that get created. Tells you what the graph's connective tissue looks like. |
| `query_top1`  (kind = 1) | One row per `query` call when there's at least one candidate. | Distribution of "best vector hit" cosines. Tells you what your queries look like. |

The samples are stored in `similarity_samples (ts, kind, cosine)`. Nothing else writes there.

### How to read the distributions

- A **healthy `query_top1` median between 0.6 and 0.8** means most queries find a strong-enough candidate to trigger a STRONG hit. Below 0.5 means the corpus is sparse for your query language. Above 0.9 means it's dense and probably has near-duplicates.
- A **healthy `insert_topk` distribution** is wide: a p25 around `0.5` and a p99 around `0.95`. If p25 and p99 are close together, every node looks like every other node — your titles are too generic, or the corpus is dominated by one topic.

If you change `cache.weak_hit_min_vec` or `cache.strong_hit_min_lex`, look at the percentiles again afterward to confirm you didn't push the gate past the bulk of real hits.

---

## `graft consolidate`

A **safe** maintenance pass. Never destructively merges semantic content — that part is intentionally manual.

What it does (`src/storage/storage.c::mg_storage_consolidate`):

1. **Prune expired** — delete nodes whose `expires_at` is non-zero and past.
2. **Drop orphan edges** — edges whose `src` or `dst` no longer exist (cascades should prevent this; the pass catches legacy rows).
3. **Drop duplicate edges** — edges that violate the `(src, dst, kind, COALESCE(keyword_id, -1))` unique index.
4. **Drop orphan `node_keywords`** — rows pointing at deleted nodes / keywords.
5. **Drop invalid edges** — weights outside `[0, 1]` or kinds outside the enum.
6. **`ANALYZE`** — refresh planner statistics so the next round of queries picks good indexes.
7. **Report** — counts (`n_nodes`, `n_edges`, `n_keywords`), isolated nodes, physically bidirectional pairs, contradictions found.

Report shape:

```json
{
  "deduped":              42,
  "contradictions_found": 0,
  "stale_marked":         0,
  "maintenance": {
    "expired_deleted":              3,
    "duplicate_edges_deleted":      42,
    "orphan_edges_deleted":         5,
    "orphan_node_keywords_deleted": 0,
    "invalid_edges_deleted":        0,
    "sqlite_analyzed":              true
  },
  "graph": {
    "n_nodes": 1281,
    "n_edges": 5570,
    "n_keywords": 312,
    "isolated_nodes": 14,
    "physical_bidirectional_pairs": 18
  },
  "recommendations": [
    "isolated nodes found: consider adding keywords or linking them through future consolidation passes"
  ],
  "note": "consolidate completed safe maintenance and graph-health analysis; semantic content merges are intentionally not destructive"
}
```

### When to run

- After importing or merging a profile.
- After a `learn` session that ingested hundreds of nodes.
- Periodically — every 100 inserts is a reasonable rhythm.

It's safe to run while the daemon is up. The pass takes one short transaction.

---

## The usage log (`~/.graft/usage.jsonl`)

Every CLI invocation appends one line of JSON to a log file:

```jsonl
{"ts":1715520000123,"op":"query","status":0,"latency_ms":42,"hit":"STRONG","id_hex":"019e..."}
{"ts":1715520003044,"op":"insert","status":0,"latency_ms":76,"hit":"","id_hex":"019f..."}
{"ts":1715520011213,"op":"retrieve","status":0,"latency_ms":58,"hit":"","id_hex":""}
```

Schema (one JSON object per line, newline-delimited):

| Field | Type | Meaning |
| ----- | ---- | ------- |
| `ts`         | int   | Unix ms when the CLI call started. |
| `op`         | string | `insert`, `query`, `retrieve`, `explore`, `get`, `delete`, `classify`, `stats`, `consolidate`. |
| `status`     | int   | Daemon return code (`0` = success). |
| `latency_ms` | int   | Total CLI round-trip including auto-start cost on the first call. |
| `hit`        | string| `"STRONG"`, `"WEAK"`, `"MISS"`, or `""` if not applicable. |
| `id_hex`     | string| The node id involved (insert → new id; query → STRONG hit id; otherwise blank). |

Path resolution:

1. `$GRAFT_USAGE_LOG` if set,
2. else `$GRAFT_HOME/usage.jsonl`,
3. else `~/.graft/usage.jsonl` (POSIX) or `%LOCALAPPDATA%\graft\usage.jsonl` (Windows).

The log is **append-only**. The CLI never reads or rotates it. Truncate it manually if it gets huge (it won't; one line per invocation is tiny).

---

## `graft analytics`

CLI-only — does **not** touch the daemon. Aggregates `~/.graft/usage.jsonl`:

```bash
graft analytics                       # last 7 days, default
graft analytics --since 24h
graft analytics --since 30d --seconds-per-hit 90
```

What you see (output is JSON):

- `period`: parsed `since` window.
- `total_calls`, `by_op`: counts.
- `latency`: `p50` / `p95` per op.
- `hit_rate`: STRONG / WEAK / MISS share of `query` calls.
- `est_seconds_saved`: STRONG hits × `--seconds-per-hit` (default 60). A coarse proxy for "agent reasoning time avoided".
- `top_authors`: number of inserts per author (from `usage.jsonl`'s `id_hex` joined to the daemon via `graft get`).

The `seconds-per-hit` knob is your guess at "how long would a fresh agent have spent figuring this out from scratch?". Pick a value that reflects your workload — 60 for trivia, 300 for hard questions.

---

## What good maintenance looks like (Claude Code / Codex flow)

The shipped `memory-audit` skill is built around this rhythm:

1. `graft stats` → eyeball the percentiles.
2. `graft analytics --since 7d` → check hit-rate, top reused nodes, never-reused nodes.
3. `graft consolidate` → run if `n_nodes` has grown more than ~100 since last audit.
4. If the report flags `isolated_nodes` → review them; usually they need a keyword.
5. If the report flags `physical_bidirectional_pairs` → no action needed; just informational.
6. If the report flags `contradictions_found` → review and resolve via supersession.

The skill is read-only by default; every proposed action is presented for user approval, not applied automatically.

---

## Debugging

If something looks wrong, look here in this order:

1. **`~/.graft/memgraphd.err.log`** — the daemon's stderr. Schema errors, model-load failures, socket errors all land here.
2. **`~/.graft/memgraphd.out.log`** — stdout. Less useful but worth a glance.
3. **`graft stats`** — gives you the counts and percentiles to confirm "is the daemon talking to the right DB?".
4. **`graft profile current`** — confirms which profile you're on. Most "where did my data go" reports are profile mismatches.
5. **The usage log** — last 50 lines tell you what the CLI has been doing.

If the daemon is unresponsive:

```bash
pkill graftd        # POSIX
Stop-Process -Name graftd   # PowerShell
```

The next CLI call will auto-start a fresh daemon.

---

## What's missing and how to improve it

- **Log rotation.** `usage.jsonl` grows without bound. A small rotation helper (`graft logs rotate`) and / or a daily cap with a sidecar file would be cleaner than "truncate manually".
- **Structured daemon logs.** The daemon uses `fprintf(stderr, ...)`. Switching to one-JSON-line-per-event would let Loki / Vector ingest it without parsing.
- **Health endpoint with more substance.** `GET /v1/healthz` returns just `{"status":"ok"}`. Adding `n_nodes`, `uptime_ms`, `last_consolidate_at` would help dashboards.
- **`graft consolidate --dry-run`** that lists candidate prunes / dedupes without applying them.
- **A real content-consolidation pass.** Today the pass is safe-only. A future `--merge-similar` mode (with confirmation) would close the loop on "two nodes about the same thing accumulated separately".
- **Stale-mark heuristics.** `state = STALE` exists in the schema but nothing assigns it today. A pass that flags nodes whose `last_access` is older than N days and `access_count == 0` would surface unused memories worth deleting.
