---
name: memgraph
description: Persistent graph memory for the agent. Use BEFORE writing solutions to check if the same problem was solved before; AFTER solving novel problems to save what you learned. Searches via semantic + lexical hybrid. Operations include query (cache lookup), retrieve (top-k), explore (related), insert (save), classify (suggest keywords), get (by id), stats.
---

# memgraph — Persistent agent memory

You have access to a long-term memory system stored as a graph. Use it to **remember solutions across conversations**.

## When to use this skill

**TRIGGER (high priority)** — invoke without asking the user when:
- The user describes a non-trivial technical problem (bug, design question, library quirk).
- You're about to write a substantial solution and similar work might already exist in memory.
- The user references "we did X before" / "you remember when…".
- After successfully solving a non-obvious problem the user might hit again.

**SKIP** when:
- The request is trivial (typo fix, rename, simple syntax).
- Memory is irrelevant (pure code execution, file listing).

## Setup

The skill assumes:
- `memgraphd` is running, listening on the socket from `MEMGRAPH_SOCKET` (default `/tmp/memgraph.sock`).
- The `memgraph` CLI is on PATH (or use absolute path to `build/memgraph`).

Check with `memgraph stats`. If it errors, ask the user to start the daemon (`./build/memgraphd --config ./config.example.yaml &`).

## Core flow: search → decide → maybe save

### 1. Search before solving

For any non-trivial question, first run:
```bash
memgraph query "<succinct restatement of the user's problem>"
```

Read the response:
- `"hit": "STRONG"` → there's a near-exact match. Use the `summary` and `detail`. Don't redo the work; cite it ("In a previous session we found that…") and apply it.
- `"hit": "WEAK"` → similar but not identical. Use `summary` as a hint; consider `memgraph get <id_hex>` to fetch the full detail before deciding.
- `"hit": "MISS"` → the `fallback_retrieve` field has up to N RRF-ranked nodes that may be related. Skim them; they may contain partial answers.

For broader exploration, use:
```bash
memgraph retrieve "<problem statement>"      # top-k hybrid
memgraph explore "<topic>" --keyword X --keyword Y   # graph walk
```

### 2. Save after solving (only when worth it)

After you and the user successfully resolve a non-trivial problem, save it:

```bash
# First, classify to get suggested keywords:
memgraph classify --summary "<one-line restatement>"

# Then insert:
memgraph insert \
  --summary "<title-style 1 line; the kind of thing future-you searches for>" \
  --detail  "<the actual answer / fix / pattern, including the WHY>" \
  --keyword <kw1> --keyword <kw2> --keyword <kw3>
```

**What to save**:
- A bug + its fix where the fix wasn't obvious from the error.
- An API quirk / framework gotcha.
- An architectural decision and the reason behind it.
- A working incantation for a hard-to-Google command.

**What NOT to save**:
- Trivial / one-off / obvious answers.
- User secrets, tokens, identifying info.
- Conversation chit-chat or planning notes.

### 3. Idempotency

`insert` deduplicates by content hash. Re-saving the same `summary+detail+keywords` returns `"duplicate": true` with the existing id — you don't need to check first.

## Quick reference

| Goal                                | Command                                                                |
| ----------------------------------- | ---------------------------------------------------------------------- |
| Cache lookup (best match)           | `memgraph query "<text>"`                                              |
| Top-k hybrid search                 | `memgraph retrieve "<text>" --top-k 10`                                |
| Graph walk from keywords            | `memgraph explore "<text>" --keyword foo --depth 3`                    |
| Save knowledge                      | `memgraph insert --summary S --detail D --keyword K`                   |
| Suggest keywords                    | `memgraph classify --summary "<text>"`                                 |
| Fetch by id                         | `memgraph get <hex_id>`                                                |
| Inspect distributions               | `memgraph stats`                                                       |

## Reading the JSON-ish output

The CLI prints a pseudo-JSON structure with `status` (0 = ok), `result` (op-specific), and on error `error` (string). Parse the relevant field; the schema is documented in `plans/sub_task_*.md`.

## Failure modes

- **`connect failed`**: daemon down. Tell the user: `./build/memgraphd --config ./config.example.yaml &`.
- **`status: 5` (MG_ERR_EMBED)**: model file missing or unreadable. Check `models/bge-m3.gguf`.
- **Empty `nodes`/`results`**: memory is genuinely empty for this query. Don't fabricate; just proceed and consider saving the eventual solution.
