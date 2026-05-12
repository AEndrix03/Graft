---
name: graft
description: >-
  Persistent graph memory across conversations. The master skill — search BEFORE answering non-trivial questions, save AFTER solving non-obvious ones. Three companion skills handle the heavy lifting: `/memoryze` (save with smart granularity), `/recall` (escalating search query→retrieve→explore), `/memory-audit` (read-only health check). The daemon auto-starts on first command. Multi-tenant via profiles. Use this skill ALWAYS for any non-trivial technical question, framework quirk, design decision, or learned-the-hard-way fix.
---

# graft — Persistent agent memory

You have a long-term memory graph that persists across conversations and across agents. Treat it as your **first stop** for anything non-trivial: another past-you (or another agent on the team) may have already solved it, and the answer lives there.

## When to engage — be aggressive

**ALWAYS check the graph (no permission needed) when ANY of these holds:**

- The user describes a technical problem, bug, error, or design decision.
- The user asks "how do I…", "why does…", "what's the right way to…".
- The user says "ricordi", "we did this before", "abbiamo già fatto".
- You're about to write more than ~30 lines of code or make an architectural choice.
- You hit a library/framework/CLI quirk (Spring, Angular, Docker, git, npm, …).
- You finish solving a non-obvious problem (search-then-save loop closes).

**Skip only when:**

- Pure mechanical edits (rename a variable, fix a typo, format).
- File listings, "what's in this folder", trivial reads.
- The question is fully answered by code already on screen.

When in doubt: **search**. A `graft query` is cheap (tens of ms warm, ~100-300ms cold). The cost of NOT checking and re-doing past work is much higher.

## Companion skills — what to invoke when

Use the right skill for the action; don't reinvent inside this one.

| Slash command       | When to use                                                                                       |
| ------------------- | ------------------------------------------------------------------------------------------------- |
| `/recall <query>`   | Looking something up. Picks query/retrieve/explore for you and escalates on weak hits.            |
| `/memoryze <hints>` | Saving knowledge **from the current conversation** (1-5 atomic nodes, smart granularity).          |
| `/learn <prompt>`   | Bulk ingestion **from external sources** (folders, codebases, doc trees). Plan-first, 10-200 nodes. |
| `/memory-audit`     | Periodic health check. Read-only; produces a report and an action menu.                           |

`/memoryze` vs `/learn` rule of thumb: source is the conversation → `/memoryze`; source is files outside the conversation → `/learn`.

This skill (`graft`) covers everything else: the raw CLI, profile management, troubleshooting, and the underlying conceptual model.

## Setup

Nothing to configure. The CLI auto-starts `graftd` if it isn't running — first command pays ~1-2s for cold-start, subsequent calls are fast.

Standard install layout (created by `scripts/install.sh` / `scripts/install.ps1`):

```
~/.graft/
├── bin/         graft + graftd + DLLs/so
├── models/      bge-m3.gguf (~600 MB)
├── config.yaml  daemon config (absolute model path)
├── profiles/<name>/graft.db   one DB per profile
├── sockets/     per-profile UNIX socket (Windows; POSIX uses /tmp)
└── graftd.log
```

If the CLI errors with `connect failed: …` AND `auto-start failed: …`, the second line tells you why (binary missing, model missing, port conflict). Surface it verbatim to the user.

## Profiles — multi-tenant memory

A profile = its own DB + its own daemon. Default profile is `default` (auto-created, not removable).

```bash
graft profile list                              # show all + active
graft profile current                           # quick check
graft profile add work                          # create new
graft profile remove work                       # delete (asks confirmation; pass --yes to skip)
graft profile export work --path work.graftprofile # backup (file is a valid SQLite DB)
graft profile import --name work2 --file work.graftprofile [--force]
```

**Active profile resolution**: `$GRAFT_PROFILE` env, else `default`. There is no global state file.

To switch profile in the current shell:

```bash
eval "$(graft profile set work)"        # bash/zsh/fish — auto-detects
graft profile set work | iex            # PowerShell
```

To make it persistent: copy the printed export line into your shell rc (`.bashrc`, `.zshrc`, `profile.ps1`).

For one-off cross-profile operations:

```bash
GRAFT_PROFILE=work graft query "deployment workflow"
```

## CLI reference (advanced)

| Goal                                  | Command                                                              |
| ------------------------------------- | -------------------------------------------------------------------- |
| Cache lookup with STRONG/WEAK/MISS    | `graft query "<text>"`                                            |
| Top-k hybrid (lex + vec via RRF)      | `graft retrieve "<text>" --top-k 10`                              |
| Graph walk from keywords              | `graft explore "<text>" --keyword foo --depth 3`                  |
| Save knowledge                        | `graft insert --title S --body D --keyword K`                 |
| Suggest keywords for a title        | `graft classify --title "<text>"`                               |
| Fetch node by id                      | `graft get <hex_id>`                                              |
| Delete node by id                     | `graft delete <hex_id>`                                           |
| Distribution percentiles              | `graft stats`                                                     |
| Usage report (hit-rate, est. saved)   | `graft analytics [--since 7d] [--seconds-per-hit 60]`             |
| Profile management                    | `graft profile <list\|current\|add\|remove\|set\|export\|import>` |

`insert` is idempotent: same `title+body+keywords` returns `"duplicate": true` with the existing id.

## Output schema (envelope)

Every command prints JSON-ish:

```json
{ "status": 0, "result": { ... } }                    // ok
{ "status": <n>, "error": "...", "result": null }     // err
```

Exit codes: `0` ok, `1` transport/encode failure, `3` handler returned non-zero status. Operation-specific schemas are documented in the project's `plans/sub_task_*.md` files.

## Failure modes — what to tell the user

| Symptom                                 | Cause                              | Action                                                                |
| --------------------------------------- | ---------------------------------- | --------------------------------------------------------------------- |
| `connect failed` + `auto-start failed`  | Binary/model missing               | Run `bash scripts/install.sh` or `pwsh scripts/install.ps1`.          |
| `status: 5` (MG_ERR_EMBED)              | Model file unreadable              | Re-check `~/.graft/models/bge-m3.gguf`.                        |
| Empty `nodes` / `results`               | Graph empty for this query         | Don't fabricate — proceed and consider `/memoryze` of the solution.   |
| `daemon spawned but socket not ready`   | Daemon crashed silently            | On Windows, MSYS2 DLLs may be missing — re-run install.sh.            |
| `profile X is currently in use`         | Daemon for that profile is up      | `pkill graftd` (POSIX) / `Stop-Process graftd` (Win), then retry. |

## Save / search ground rules

The companion skills enforce these, but they bear repeating:

**Save when**:
- Bug + non-obvious fix.
- Library/framework/CLI quirk or gotcha.
- Architectural decision and **why** it won.
- Working incantation for a hard-to-Google command.
- "From now on we always X" standards.

**DO NOT save**:
- Trivial / one-shot / obvious answers.
- User secrets, tokens, internal URLs.
- Conversation chit-chat or planning notes.
- Anything derivable from current code/git.

**Search before answering**: any non-trivial problem gets a `/recall` first. Cite the result if STRONG. Don't re-derive what's already there.

## Measuring usefulness

Run `/memory-audit` periodically (or after a long session). The graph is paying off when:

- `hit_rate >= 0.30` (mature graph).
- `insert_to_query_ratio < 1.0` (more reads than writes).
- A small number of nodes account for most STRONG hits (Pareto = good; means high-leverage knowledge is concentrated).

If those signals look bad, the typical fix is **better summaries** (the retrieval anchor) — re-save with phrasing that matches how you'd search, not how you solved it.

## When you find a wrong / obsolete node

If a `/recall` or a `get` surfaces a node whose content you can verify is **wrong** (stale, contradicted by current code, factually incorrect), you have two paths:

1. **Remove only** — the node is just noise, no replacement needed:
   ```bash
   graft delete <hex_id>
   ```

2. **Modify (= delete + re-insert)** — the underlying knowledge is real but the saved node is broken:
   ```
   1. fetch:    graft get <hex_id>
   2. redesign: write new title / body / keywords
   3. delete:   graft delete <hex_id>
   4. re-save:  graft insert --title S --body D --keyword K1 --keyword K2 …
   ```
   The insert pipeline rebuilds embedding + edges automatically. Content-hash dedup means re-running the same insert is safe (returns the existing id).

**Don't** silently leave wrong nodes in the graph hoping they won't get retrieved — they will, and they'll mislead future-you. Trust the agent's verdict when it says "this is contradicted by current code".

When in doubt (the node looks suspicious but you're not sure), **demote** rather than delete: re-save with a `wip` or `unsure` keyword and a body that flags the uncertainty.

## Skill chain in a typical session

1. User asks a non-trivial question → you invoke `/recall`.
2. STRONG hit found → cite and apply.
3. MISS → solve normally; once converged, invoke `/memoryze` to save the result.
4. Periodically (start of a long session, end of a sprint) → `/memory-audit` to spot drift.

Don't sprinkle these skills inside one another's bodies; they're meant to be invoked at the right level. This master skill is the coordinator.
