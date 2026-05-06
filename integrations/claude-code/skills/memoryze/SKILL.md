---
name: memoryze
description: Distill the current conversation (or a specified excerpt) into N high-quality memgraph nodes and save them. Triggered by `/memoryze`, "save this to memory", "ricorda questo", "memorize this", or whenever the user explicitly wants the agent to commit knowledge to the persistent graph. The user can hint at granularity ("split into 3 atomic nodes", "one comprehensive node"), focus ("save the WHY, not the diff"), and target profile. Auto-classifies keywords. Prefer this over a raw `memgraph insert` whenever you're saving more than a one-liner.
---

# memoryze — Distill and persist conversation knowledge

You are about to commit something the user solved/decided to long-term memory. This skill is the **shape** of that act: pick the right granularity, write summaries that future-you will actually find, and avoid garbage-in.

## Argument shape

The user invokes you with a free-form prompt that may include:

| Hint                                | Example                                                              | What it means                                                                |
| ----------------------------------- | -------------------------------------------------------------------- | ---------------------------------------------------------------------------- |
| **count / granularity**             | "3 nodi", "atomic", "one big node", "split per business rule"        | How many nodes to produce. Default: heuristic (see below).                   |
| **focus**                           | "the WHY, not the diff", "include the workaround", "skip syntax"     | What aspect of the solution to emphasize in `detail`.                        |
| **scope**                           | "this conversation", "from the bug fix onwards", "only the SQL part" | Which slice of context to distill.                                           |
| **target profile**                  | "in the work profile", "in test"                                     | Switch via `MEMGRAPH_PROFILE` for the inserts.                               |
| **enrichment**                      | "expand with the official docs link", "add the error message"        | Inject extra data the agent should look up before saving.                    |

Examples:

- `/memoryze the spring boot @Valid cascade fix, 1 node, focus on WHY`
- `/memoryze split this debugging session into atomic nodes per hypothesis tested`
- `/memoryze le ultime 10 messaggi su Angular signal reactivity, 3 nodi`

If hints are absent, ask **at most one** clarifying question, then proceed with the default heuristic.

## Default granularity heuristic

Count the **independent decisions** in the slice the user pointed at:

- 1 node when the slice is a single bug→fix, single design choice, single quirk.
- N nodes when the slice contains N orthogonal facts (e.g. multiple unrelated rules in one config refactor).
- Never more than ~5 nodes from one `/memoryze` invocation — beyond that, the user should narrow scope or run multiple invocations.

**Atomicity test**: each node should be retrievable on its own when future-you searches for it. If two prospective nodes would always co-occur in the answer, merge them.

## Per-node shape

Every saved node has three required fields plus 3-6 keywords. Quality of these fields *is* the skill — bad shape means future search misses.

### `summary` (one line, ~80-120 chars)

Write the line **future-you will type into search**, not a title.

- Bad: `"fixed the bug"` — useless.
- Bad: `"FIX-2024-12 spring boot cascade"` — search-by-ticket only.
- Good: `"Spring Boot DTO nested validation needs @Valid on field plus @Validated on controller"`
- Good: `"Angular signal-based form: canConfirm reattivo via computed(), non property"`

The summary IS your retrieval anchor. Phrase it the way the problem appears, not the way the solution looks.

### `detail` (1-20 lines, prose + code if relevant)

Include:
- **The WHY** — what made this non-obvious, what was tried first and didn't work.
- **The minimum reproducing context** — a code snippet or command, never a whole file.
- **The trap** — what about this would mislead someone who doesn't know it.
- **References** — issue numbers, PR URLs, official doc links the user mentions.

Skip:
- Conversational chit-chat ("then we tried...", "ok perfect").
- Personal opinions unless they're the *decision* itself.
- Anything that's already obvious from the summary.

### `keywords` (3-6)

Run `memgraph classify --summary "<your summary>"` first. The system suggests keywords from existing graph keywords when possible, infers when novel. Use the suggestions verbatim **unless** they miss a critical axis (e.g., classify gave you `[spring-boot, validation]` but the node is also a `gotcha` worth flagging — add it).

Conventional axis keywords to consider adding:
- `gotcha` — non-obvious traps the docs don't mention.
- `standard` — "from now on we always do X" decisions worth reusing across projects.
- `incident` — links to a real production breakage.
- `workaround` — temporary fix while the real issue is open elsewhere.

## Workflow

For each node to be created (1 to N):

```bash
# 1. Suggest keywords
memgraph classify --summary "<the summary you drafted>"

# 2. Insert. Idempotent — duplicate hashes return the existing id_hex.
memgraph insert \
  --summary "<final summary>" \
  --detail  "<final detail>" \
  --keyword <kw1> --keyword <kw2> --keyword <kw3>
```

After all inserts, **report back to the user**:
- The IDs (`id_hex`) created and which were duplicates of existing nodes.
- Suggest a follow-up `/recall` to verify the saved nodes are findable with the user's likely future queries.

## Refusals

Do NOT save:
- Secrets / credentials / tokens / internal URLs that look sensitive.
- The user's personal information (full name, email, phone) unless explicitly part of the knowledge.
- Conversation transcripts — distill, don't archive.
- Information the user said is "experimental / not committed" / "may be wrong".

If asked to save something refused, explain why in one line and propose a redacted version the user can confirm.

## Profile targeting

If the user said "in the X profile" / "nel profilo X":

```bash
MEMGRAPH_PROFILE=X memgraph insert ...
```

Don't permanently switch the user's shell — pass the env var on the line. Tell the user which profile the inserts went into.

## Output format to the user

End with a compact, scannable summary:

```
Saved to profile=<name>:
  ✓ <id_hex_short> — <summary>
  ✓ <id_hex_short> — <summary>
  • <id_hex_short> — <summary>      (duplicate, not re-saved)

Try: memgraph query "<one of the summaries>"
```

Keep this terse. The user already knows what they asked you to save — they want confirmation, not an essay.
