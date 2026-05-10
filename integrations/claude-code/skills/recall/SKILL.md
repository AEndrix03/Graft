---
name: recall
description: Smart graft search that picks the right tool (query → retrieve → explore) based on the question's shape, escalates when results are weak, and presents findings ranked by confidence with the originating evidence. Triggered by `/recall`, "do we have something about X", "have we seen this before", "ricordi se", "search the graph". Prefer this over a raw `graft query` whenever the user is exploring rather than confirming a known answer.
---

# recall — Smart, escalating search of the memory graph

`graft` exposes three search modes and they have different sweet spots:

| Mode       | Best when                                                                  |
| ---------- | -------------------------------------------------------------------------- |
| `query`    | The user is asking for the answer to a specific problem. Cache-style gating: STRONG / WEAK / MISS. |
| `retrieve` | The user is exploring; they want top-K hybrid (lexical + semantic) ranked results. |
| `explore`  | The user names a topic + keywords; they want to walk the graph from there. |

This skill orchestrates them. **Do not ask the user which mode** — pick based on the question.

## Argument shape

The user invokes you with a free-form question or topic.

| Pattern                                              | Strategy                                              |
| ---------------------------------------------------- | ----------------------------------------------------- |
| "how do I X" / specific problem statement            | `query` → escalate to `retrieve` if MISS.             |
| "what do we know about X" / open-ended topic         | `retrieve --top-k 10`.                                |
| "X with Y" / topic with keyword anchors              | `explore "X" --keyword Y`.                            |
| "find related to <id>"                               | `explore` from that node's keywords (read via `get`). |
| "recent stuff about X"                               | `retrieve` and re-rank by node `created_at` if shown. |

If the user provides a `--keyword` style flag, respect it.

## Cascade flow

```
                                      ┌─── STRONG → done; cite + use ───┐
   graft query <Q>  ──────────────┤                                   │
                                      ├─── WEAK   → graft get <id>    │── present
                                      │             then continue        │
                                      └─── MISS   → graft retrieve <Q>│
                                                    if 0 useful results: │
                                                    graft explore <Q>│
                                                    --keyword <inferred>│
```

### Step 1 — `query`

```bash
graft query "<concise restatement of the user's question>"
```

Read `result.hit`:

- **STRONG** — high confidence near-exact match. Output: cite the node (`title`, `body`), state explicitly "this is from a previous session", and stop. Do NOT re-derive the answer.
- **WEAK** — similar but not identical. Fetch the full body with `graft get <id_hex>`, present it labeled as "WEAK match — review before using". Then proceed to Step 2 to find better candidates.
- **MISS** — go to Step 2.

### Step 2 — `retrieve`

```bash
graft retrieve "<question>" --top-k 10
```

Read each result's `title`. Drop those with low `score`. If 1-3 are clearly relevant: present them ranked. If 0 useful: go to Step 3.

### Step 3 — `explore`

Pick 1-3 keywords from the question. Use the same vocabulary the graph likely uses (run `graft stats` once if you've never seen this graph; the keyword distribution is implicit there). Then:

```bash
graft explore "<question>" --keyword <kw1> --keyword <kw2> --depth 3 --beam 4
```

If still nothing: tell the user explicitly "the graph has nothing on this", **don't fabricate**. Suggest they consider `/memoryze`-ing the eventual solution.

## Cross-profile reach

If the user says "search across profiles" or you suspect the answer might be in a different profile:

```bash
for p in $(graft profile list | jq -r '.profiles[]'); do
  echo "=== $p ==="
  GRAFT_PROFILE=$p graft query "<question>"
done
```

Default behavior: stay in the current profile. Cross-profile is opt-in.

## Output format to the user

Present in **descending confidence**:

```
STRONG (cache hit, profile=work):
  → <title>
    <body trimmed if very long>
    [id: <short_hex>]

WEAK (1 candidate, score 0.78):
  ~ <title>  ← review before reusing
    [id: <short_hex>]

Top-K (retrieve fallback, profile=work):
  • <title>  (score 0.62)
  • <title>  (score 0.55)
```

Avoid wall-of-JSON output. The CLI prints JSON; your job is to summarize it for the human.

## Post-recall hooks

- If the result was used as the basis for a new solution that diverges meaningfully, **suggest** `/memoryze` of the new variant.
- If there were multiple WEAK candidates that all looked redundant, suggest `/memory-audit` — the graph may be accumulating duplicates.
- If `query` returned MISS but `retrieve` had a great hit, that means the saved title doesn't match how the user phrases this. Suggest the user re-save with a better title, or do it yourself with their permission.
