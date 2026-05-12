---
name: graft-init
description: >-
  One-shot configurator that wires graft into a CLAUDE.md (global or project-local) so the agent uses the persistent memory consistently in every session. Asks the user 4 short questions (scope, caching, retrieve strategy, save strategy), then writes a `<!-- graft:start -->` … `<!-- graft:end -->` block into the chosen CLAUDE.md. Triggered by `/graft-init`, "configura graft", "set up graft for this project", "enable graft globally". Idempotent — re-running it replaces the previous block in place.
---

# graft-init — Configure graft behavior in CLAUDE.md

This skill writes a short, opinionated instruction block into a CLAUDE.md so future sessions automatically use the graft skills (`recall`, `memoryze`, `learn`, `memory-audit`) according to the user's preferences. The block is fenced with HTML markers so re-running the skill cleanly replaces it.

## Flow

Run the four `AskUserQuestion` calls **in order**. Stop immediately if the user picks `exit` on Q1. Pass `multiSelect: false` for all four. Defaults are marked **(recommended)** in the option labels — do not auto-pick, the user must choose.

### Q1 — Scope

```
question: "Where should the graft configuration live?"
header:   "Scope"
options:
  - { label: "Global (recommended)", description: "Write to ~/.claude/CLAUDE.md — applies to every project on this machine." }
  - { label: "Local",                description: "Write to ./CLAUDE.md in the current repo — applies only here, takes precedence over global." }
  - { label: "Exit",                 description: "Cancel without changing anything." }
```

If the user picks **Exit**, reply with one short line ("Cancelled, nothing changed.") and stop.

### Q2 — Caching

```
question: "Enable cache-first lookups?"
header:   "Caching"
options:
  - { label: "Enable (recommended)", description: "Use `graft query` first (fast, exact-match cache) before escalating to retrieve. Cuts latency on repeat questions." }
  - { label: "Disable",              description: "Always go straight to retrieve. Slower but always semantic." }
```

### Q3 — Retrieve strategy

```
question: "When should the agent search the graph during a conversation?"
header:   "Retrieve"
options:
  - { label: "Every interaction on cache miss (recommended)", description: "Run `/recall` on any non-trivial turn whenever the cache lookup misses. Maximum coverage." }
  - { label: "Only at conversation start",                    description: "One opening recall to load context, then rely on conversation memory." }
  - { label: "Never",                                         description: "Do not retrieve automatically — only when the user explicitly asks." }
```

### Q4 — Save strategy

```
question: "When should the agent persist new knowledge with /memoryze?"
header:   "Save"
options:
  - { label: "Per task milestone (recommended)", description: "Save as soon as a non-obvious sub-problem is solved. Highest recall later, no end-of-session loss." }
  - { label: "End of conversation only",         description: "Bundle everything into a single save when the work wraps up." }
  - { label: "Never",                            description: "Do not save automatically — only when the user asks." }
```

## Writing the block

Resolve the target file:

- Global: `$env:USERPROFILE\.claude\CLAUDE.md` (Windows) or `~/.claude/CLAUDE.md` (POSIX). Create the parent dir if missing. Create the file if missing.
- Local: `./CLAUDE.md` in the cwd. Create if missing.

Build the block from the chosen options. Use these exact markers so future runs can find and replace it:

```markdown
<!-- graft:start -->
## graft — persistent memory (auto-configured by /graft-init)

You have a persistent memory graph (`graft`) shared across sessions. Use it actively — it is your first stop on non-trivial work and the place where hard-won knowledge accumulates.

**Retrieve:** {{retrieve_clause}}
**Cache:** {{cache_clause}}
**Save:** {{save_clause}}

Companion skills: `/recall <q>` (search), `/memoryze <hints>` (save from conversation), `/learn <prompt>` (bulk-ingest external sources), `/memory-audit` (health check). Prefer these slash commands over raw `graft` CLI calls.

Skip retrieval/save only for purely mechanical edits (rename, typo, format) or questions fully answered by code already on screen.
<!-- graft:end -->
```

Substitute the clauses based on Q2/Q3/Q4 answers:

| Choice | `retrieve_clause` |
| ------ | ----------------- |
| Every interaction on cache miss | "On every non-trivial user turn, attempt a cache lookup; if it misses, run `/recall` before answering." |
| Only at conversation start | "At the start of each conversation, run one `/recall` pass to load relevant context. Re-run only if the topic shifts substantially." |
| Never | "Do not retrieve automatically. Use `/recall` only when the user explicitly asks." |

| Choice | `cache_clause` |
| ------ | -------------- |
| Enable | "Use `graft query` first for fast exact-match lookups; escalate to `retrieve` / `explore` only on miss." |
| Disable | "Skip the cache layer; go straight to semantic `retrieve`." |

| Choice | `save_clause` |
| ------ | ------------- |
| Per task milestone | "Save with `/memoryze` as soon as a non-obvious sub-problem is solved — do not wait for end of session." |
| End of conversation only | "At the end of the conversation, run `/memoryze` once to persist what was learned." |
| Never | "Do not save automatically. Use `/memoryze` only when the user explicitly asks." |

## Idempotency

Read the target file first.

- If it contains `<!-- graft:start -->` … `<!-- graft:end -->`, replace **exactly that span** (markers included) with the new block. Do not touch anything else in the file.
- Otherwise, append the block to the end of the file with a single blank line separating it from the previous content. If the file is empty/just-created, write the block as-is.

Never duplicate the block. Never strip user content outside the markers.

## After writing

Reply in **two short lines**:

1. `Wrote graft config to <absolute path>.` (one line)
2. A one-sentence recap of the chosen retrieve + save strategy so the user can confirm at a glance.

Do not paste the block back. Do not add explanations beyond those two lines unless the user asks.

## Failure modes

- User picks Exit on Q1 → stop, no file touched.
- Target dir cannot be created (permissions) → report the path and the error verbatim, do not retry blindly.
- File exists but is read-only → report and stop; do not chmod without asking.
