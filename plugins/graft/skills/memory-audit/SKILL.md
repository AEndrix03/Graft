---
name: memory-audit
description: >-
  Health check + maintenance audit of the graft graph. Reports hit rate, hoarding ratio, top reused nodes, never-reused nodes, stale entries, and similar-but-separate clusters that may be duplicates. Suggests concrete actions (re-save with a better title, promote to README, drop stale, narrow over-broad nodes) but does NOT modify the graph automatically — every action is proposed for the user to approve. Triggered by `/memory-audit`, "is the graph healthy", "audit memory", "check graft quality", or whenever the user wants a cleanup pass before a long-running session.
---

# memory-audit — Read-only health check + actionable suggestions

The graph rots like any knowledge base: people save sloppy summaries, save the same fact with three different keywords, save things that never get reused. This skill produces a single readable report and a punch-list of concrete maintenance actions for the user to approve.

**Never modify the graph from this skill.** It's read-only by design — propose, don't act. The user runs follow-up `/memoryze` / re-tag / delete operations themselves.

## What you collect

Run these in order and aggregate:

```bash
# 1. Distribution health
graft stats

# 2. Usage signal across the lifetime of the graph
graft analytics

# 3. Last-week trend (compare with lifetime to spot degradation)
graft analytics --since 7d

# 4. Profile context
graft profile current
graft profile list
```

If the user said "audit profile X" or "tutti i profili", iterate with `GRAFT_PROFILE=X` per profile.

## What you analyze

### A. Hit-rate health

From `analytics.cache.hit_rate`:

- **>= 0.30** — healthy mature graph.
- **0.10 to 0.30** — adequate, but may improve with better summaries.
- **< 0.10** — either the graph is too young (< 50 queries) OR something's wrong: bad summaries, or the agent isn't searching before answering.

Use `events.total` to disambiguate: < 50 events ⇒ "too young to judge"; >= 50 ⇒ real problem.

### B. Hoarding ratio

From `analytics.insert_to_query_ratio`:

- **< 0.5** — healthy (more searches than saves; the graph is being USED).
- **0.5 - 1.5** — tolerable; the agent saves and searches in roughly equal measure.
- **> 1.5** — the agent is hoarding without checking first. Tell the user: "we're saving more than searching — likely creating duplicates."

### C. Champion vs. orphan nodes

From `analytics.top_reused_nodes`:
- The top 3-5 IDs by `hits` are **champions**. For each, show:
  - `graft get <id_hex>` → the full content
  - **Suggestion**: if `hits >= 5`, consider promoting the content to a README / docs page — it's load-bearing knowledge.

For **orphans** (nodes that never got a STRONG hit), the data isn't directly available; infer indirectly:
- If `events.total > 100` and `cache.strong / events.insert < 0.2`, most inserts are never reused. Tell the user: "many saved nodes never get retrieved — either they're unfindable (bad summaries) or unused (irrelevant). Run `/recall` against a few representative ones to check."

### D. WEAK clusters (potential duplicates)

If recent activity has **many WEAK hits** (`cache.weak / cache.strong > 1`), the graph likely contains semantically-near-but-textually-different nodes. Report this and suggest:
- Pick 1-2 recent WEAK queries from memory of the conversation if any.
- For each, run `graft retrieve "<query>" --top-k 5`.
- If two of the top-5 have very similar summaries (you eyeball this — there's no built-in dedup score), flag them as a candidate merge.

### E. Latency outliers

From `analytics.latency_ms.avg_query`:

- **< 200 ms** — fine.
- **200 - 800 ms** — acceptable for a graph with cross-encoder enabled or large context.
- **> 800 ms** — investigate. Could mean huge graph + cold cache, or a misconfigured `embedding.threads`.

### F. Profile sanity

From `profile list`:

- A single profile with hundreds of nodes spanning unrelated topics → suggest splitting into per-domain profiles (`work`, `personal`, `system-foo`).
- Many profiles with < 5 nodes each → consolidate or remove unused ones.
- The `default` profile should never be removed (it's not removable anyway), but if `current != default` for the project, suggest the user persist that with `eval "$(graft profile set <name>)"` in their shell rc.

## Output: the audit report

Render a single readable report — **not** a JSON dump. Structure:

```
graft audit — profile=<name>, <N> events lifetime, last 7d: <M>

Hit rate:           <pct>%        (lifetime) / <pct>% (last 7d)   [ok | warn | bad]
Hoarding ratio:     <ratio>x      [ok | warn | bad]
Avg query latency:  <ms> ms       [ok | warn | bad]

Champions (top reused):
  ★ <id_hex_short> · hits=<n> · "<title>"
  ★ ...

Findings:
  ! <one-line problem>
    → <one-line action>
  ! ...

Suggested next steps (in order):
  1. <do this first because it has highest impact>
  2. <then this>
  3. <optional polish>
```

Keep the whole report under 40 lines. The user can ask follow-up questions for any line.

## Action menu

After the report, present a numbered action menu the user can pick from:

```
What now? Reply with a number or skip:
  [1] Run /recall against the champions to verify they're still findable
  [2] Promote champion #<id> to README/docs (I'll draft the page)
  [3] Re-save node <id> with a better title (I'll propose the rewrite)
  [4] Investigate WEAK cluster: <query>
  [5] Skip / done
```

Items 1, 2, 3 are concrete enough that you can act on them in the next turn without further input. Item 4 needs human judgment (which of the WEAK matches are duplicates).

## What this skill does NOT do

- It does **not** delete nodes by itself — even when a node is clearly wrong, the action menu proposes the deletion, the user approves, then `graft delete <id>` runs as a follow-up step.
- It does **not** re-classify keywords without explicit user approval.
- It does **not** modify the active profile.
- It does **not** export/import — the user owns those.

When you want to act on a finding, route through `/memoryze` (for re-saves with better summaries), `graft delete <id>` (when the user confirms a node should go), or instruct the user to use `graft profile export/import` for backups.
