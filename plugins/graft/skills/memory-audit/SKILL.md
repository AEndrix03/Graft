---
name: memory-audit
description: >-
  Maintenance pass + health check of the graft graph. Runs the `graft maintain` loop: checks what is due, applies the mechanical cleanup, then adjudicates every maintenance candidate (near duplicates, supersessions, contradictions, memories whose source file changed or disappeared, keyword variants, stale low-value notes) against the current code and docs and resolves it with one audited, reversible action, without asking the user in the normal path. Ends with a short report (what was resolved, hit rate, reuse). Triggered by `/memory-audit`, "is the graph healthy", "audit memory", "clean up the memory", "check graft quality", or when `graft maintain status` recommends work.
---

# memory-audit — Keep the graph true, without bothering the user

The graph rots like any knowledge base: the code moves on and a note goes stale, the same fact gets saved twice, a newer decision replaces an older one. Graft finds these cases and hands them to you as **candidates** with their evidence; **you** decide each one against the repository as it is now, and apply the decision with one narrow command. The user installed Graft so they would not have to curate memories by hand: resolve on your own, and only surface what is irreversible or genuinely ambiguous.

Every action below is recorded in `graft maintain log` and is reversible with `restore` until the retention window ends (`retire` purges after `maintenance.retention_days`, default 30). Never use `graft delete` in this skill: it is immediate and permanent.

## 1. Is anything due?

```bash
graft maintain status
```

Cheap (no embedding). Read `recommended`:

- `apply-safe` → step 2.
- `scan` or `resolve` → step 3.
- empty → nothing to maintain; skip to the report (step 5) if the user asked for an audit, otherwise stop here.

When the user explicitly asked for an audit, run steps 2-3 anyway.

## 2. Mechanical cleanup

```bash
graft maintain apply-safe
```

Safe by construction: expired nodes, orphan rows, `ANALYZE`, retired nodes past retention, and exact duplicates (same title and body) collapsed onto the oldest copy. Nothing to decide; note the counts for the report.

## 3. Adjudicate candidates

Run it from the repository the memories describe, so changed / removed file sources are detected (or pass `--root <repo>`):

```bash
graft maintain scan --limit 20
```

Candidates come most urgent first. For each one:

1. Read the nodes: `graft get <id_hex>` (the candidate already shows titles, states and dates).
2. Read the evidence it names: the file in `signals.sources[].locator`, the code or docs the note talks about, the other note of the pair.
3. Decide, then apply exactly one action:

```bash
graft maintain resolve <candidate-id> --action <action> [--by <id>] [--node <id>] --note "<one line: why>"
```

Always pass `--note`: it is the audit trail's only record of your reasoning.

| Kind | Check | Usual decision |
|---|---|---|
| `source_changed` | Does the note still hold for the file as it is now? | Still true → `refresh`. Outdated → `graft insert` the corrected note (with `--source file:<path>`), then `supersede --by <new-id>`. Not worth rewriting → `stale`. |
| `source_removed` | Was the file moved or deleted, and is the knowledge still true elsewhere? | Moved → insert with the new source, `supersede --by <new-id>`. Knowledge gone → `retire`. |
| `possible_supersession` | Does the newer note (`b`) replace the older one (`a`)? | Yes → `supersede_a`. They say different things → `keep_both`. |
| `near_duplicate` | Same fact twice? | One says it all → `supersede_a` / `supersede_b` (supersede the weaker). Both add something → insert one merged note, then `merge --by <new-id>`. Different facts → `keep_both`. |
| `contradiction` | Which side matches the code? | Wrong side → `supersede_a` / `supersede_b` so the right one replaces it, or `stale --node <wrong-id>`. |
| `keyword_fragmentation` | Does the variant spelling hurt findability? | Usually `keep`. If it matters → re-insert the note with the established keyword, `supersede --by <new-id>`. |
| `isolated_low_value` | Is the note still useful to anyone? | No → `retire`. Yes → `keep`. |

`keep` / `keep_both` dismisses the candidate until its evidence changes (new content, new file version), so it will not come back on the next scan. For pairs, `a` is always the older node.

Process the batch, then scan again: resolved and dismissed candidates do not reappear, so the loop converges. Stop after two or three batches in one turn; the rest can wait for the next session.

### When to involve the user

Only these go to the user, with the candidate and your evidence in two lines:

- you cannot tell which side of a contradiction is right from the code, docs or conversation;
- the decision depends on intent you cannot observe (a planned migration, a policy not written anywhere);
- the user asked to review decisions before they are applied.

Everything else you resolve yourself. If you are unsure between `stale` and `retire`, choose `stale`: the note stays searchable and the next pass can retire it.

### Undo

```bash
graft maintain log --limit 20                        # what was done, by whom, why
graft maintain resolve --node <id> --action restore  # back to ACTIVE
```

## 4. Usage signals (for the report)

```bash
graft stats
graft analytics --since 7d
graft analytics
```

- **Hit rate** (`analytics.cache.hit_rate`): `>= 0.30` healthy on a mature graph; `< 0.10` with more than 50 events means unfindable titles or an agent that does not search first.
- **Hoarding ratio** (`analytics.insert_to_query_ratio`): `> 1.5` means saving more than searching, which breeds the near duplicates you just resolved.
- **Champions** (`analytics.top_reused_nodes`, `hits >= 5`): load-bearing knowledge; suggest promoting it to the repository's docs.

If the user said "audit profile X" or "all profiles", repeat steps 1-4 with `GRAFT_PROFILE=X`.

## 5. Report

A short, readable summary, not a JSON dump:

```
graft maintenance — profile=<name>

Mechanical:   <n> retired purged, <n> exact duplicates collapsed, <n> expired removed
Resolved:     <n> candidates (<n> superseded, <n> refreshed, <n> retired, <n> stale, <n> kept)
Pending:      <n> (next pass)
Needs you:    <only the irreversible / ambiguous cases, one line each, or "nothing">

Hit rate:     <pct>% lifetime / <pct>% last 7d   [ok | warn | bad]
Hoarding:     <ratio>x                           [ok | warn | bad]
Champions:    <id_short> "<title>" (hits=<n>), ...
```

Keep it under 20 lines. Every resolution is in `graft maintain log` if the user wants the detail.

## What this skill does NOT do

- It does not `graft delete`: retirement is the reversible path, and `apply-safe` does the physical deletion after retention.
- It does not rewrite node content in place; a corrected note is a new insert that supersedes the old one.
- It does not change the active profile, export, or import.
