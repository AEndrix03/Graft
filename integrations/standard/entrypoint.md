# graft - long-term agent memory

This repo or assistant has access to graph-based persistent memory via the
`graft` CLI. The daemon `graftd` must be running.

## When to use

Use BEFORE solving a non-trivial technical problem to check if it is already in
memory.

Use AFTER solving a novel, non-obvious problem to save what was learned for
future sessions.

Skip trivial tasks such as typos, renames, obvious code, file listings, and
simple reads.

## Hook-aware behavior

If the current prompt already contains a `<graft-cache ...>` block, the prompt
hook has already run `graft query` for this turn.

- `hit="STRONG"`: use the injected `id_hex`, `title`, and `body` directly.
- `hit="WEAK"`: treat the injected weak title and explore candidates as leads.
  Fetch full bodies with `graft get <id_hex>` only when needed.
- `hit="MISS"`: do not repeat the same `graft query`; proceed normally, or run
  `graft retrieve` / `graft explore` only if broader context is materially
  useful.

If the prompt contains a `<graft-proposal>` block, review it before starting new
work. Save only non-obvious fixes, API quirks, or decisions; skip mechanical
edits. Decide automatically unless the content is sensitive or ambiguous.

## Core flow

### 1. Search

```sh
graft query "<problem restated>"
```

Read `result.hit`:

- `STRONG`: use the answer directly. Cite: "We solved this before..."
- `WEAK`: similar; consider `graft get <id_hex>` for the full body.
- `MISS`: use `fallback_retrieve.results[]`, `retrieve`, or `explore` only when
  useful.

```sh
graft retrieve "<text>" --top-k 10
graft explore "<text>" --keyword K1 --keyword K2 --depth 3 --beam 4
```

### 2. Save

Each saved node has this contract:

- `title`: short, searchable summary of the body.
- `body`: Markdown with context, fix/decision, why, and minimal snippets.
- `keywords`: 2-5 good keywords selected after classification.

Always classify before insert:

```sh
graft classify --title "<title>"
graft insert --title "<title>" --body "<Markdown body>" \
  --keyword K1 --keyword K2 --keyword K3
```

`insert` is idempotent. Re-saving identical title, body, and sorted keywords
returns `"duplicate": true`.

## What to save

- Bug + fix where the fix was not obvious from the error.
- API quirks / framework gotchas.
- Architectural decisions + reason.
- Hard-to-Google commands that work.

## What NOT to save

- Trivial answers, user secrets, chit-chat.
- Raw transcripts. Distill the reusable knowledge.

## Operations summary

| Op | Use case |
| --- | --- |
| `query` | Cache lookup, best-match with multi-signal gating |
| `retrieve` | Top-k hybrid lexical + semantic search |
| `explore` | Graph walk from a topic and keywords |
| `insert` | Save a new node |
| `classify` | Suggest keywords from a title |
| `get` | Fetch a specific node by `id_hex` |
| `stats` | Inspect similarity distributions |

## Health check

`graft stats` works: ready to go. If it errors with `connect failed`, the daemon
is not running:

```sh
./build/graftd --config ./config.example.yaml &
```

## Output format

CLI prints JSON: `{"status": int, "result": {...}, "error": "..."?}`.
`status: 0` means OK.
