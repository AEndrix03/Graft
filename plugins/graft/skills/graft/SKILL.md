---
name: graft
description: >-
  Persistent graph memory shared across conversations and agents. Graft is a prompter, not a cache: it hands you notes that may be close to the problem in front of you, and a close note is already a win - it gives you a starting point and reminds you what was decided before. Search it before non-trivial work, and write to it whenever the answer you just produced was not already there. Companion skills: `/recall` (search), `/memoryze` (save), `/learn` (bulk ingest; `/learn docs` for a repository's documentation; `/learn bootstrap` for the unattended cold start of a project), `/memory-audit` (maintenance pass + health check). After `/graft-init` the agent runs the whole lifecycle itself (bootstrap, recall, repair, writeback, bounded maintenance) from one cheap `graft status` call, without asking the user. The daemon auto-starts on the first command.
---

# graft - the prompter in your pocket

Picture yourself sitting an exam with a stack of small notes. Graft is that stack.
Some notes answer the question exactly. Many are merely *near* it - and those are
still worth reading: they cut the reasoning short and they remind you which
decisions were already made and why.

Two consequences, and everything else in this skill follows from them:

1. **A similar hit is a hit.** Do not demand an exact match before you use
   something. Take the near note as a starting point, then verify it.
2. **A miss is not a dead end, it is a gap.** You are the one who maintains this
   graph. When graft had nothing and you solved the problem anyway, write the
   note - so that next time the stack answers. The more notes, the more hits.

Nobody else is going to fill it. If you do not write, the graph does not grow.

## Lifecycle - running graft without being asked

After `/graft-init` the user should not have to think about graft again. You own
the whole loop, and every step of it is bounded: nothing here may turn a user's
request into a long ingestion or a maintenance session.

**Once per session in a project: `graft status`.** One cheap call (a state file,
a read-only count, at most one daemon call without embedding; it never starts the
daemon) that returns the project's bootstrap state, the maintenance status and a
`next` list, most urgent first. On Claude Code the plugin's session hook runs it
for you and hands you a one-line `graft:` hint when something is due; no hint
means nothing was due at session start (re-check after a session with many
inserts). Act on `next`:

| `action` | What to do (one bounded batch, then back to the user's work) |
| -------- | ------------------------------------------------------------ |
| `bootstrap` | First encounter (`bootstrapped: false`) or pending topics: one `/learn bootstrap` pass, the area of the current task first. Run it in a sub-agent or background task if you can; otherwise answer the user first and bootstrap after. Never before the user's answer. |
| `apply-safe` | `graft maintain apply-safe`. Mechanical and safe; just run it. |
| `resolve` | `graft maintain scan --limit 5`, adjudicate those candidates the `memory-audit` way, stop. |
| `refresh-sources` | `graft sources diff --changed-only`, revalidate the few notes whose files moved on (`graft sources refresh <id>`, or correct and supersede). |
| `scan` | `graft maintain scan --limit 20` when the session has room; otherwise leave it for the next one. |

`maintenance: null` means the daemon is not running yet: skip maintenance this
time, it will show up once the first search has started the daemon. `sources`
counts files that changed as of the last scan, not now - `graft sources diff` is
the fresh (and costlier) check.

**Before substantial work: recall.** See *When to search* below. On Claude Code
the plugin's prompt hook may already have put a `STRONG` note in your context
("graft memory: ..."); treat it like any `STRONG` hit - read it, check it.

**During work: repair what is wrong, immediately.** When a note you recalled is
contradicted by the code, the docs or the user, insert the corrected note and
supersede the old one in the same turn:

```bash
graft insert --title ... --body ... --keyword ... [--source file:<path>]   # -> <new>
graft maintain resolve --node <old> --action supersede --by <new> --note "<why>"
graft maintain resolve --node <old> --action stale --note "<why>"   # when you cannot write the correction
```

Superseding is audited and reversible (`graft maintain log`, `--action restore`);
`graft delete` is not, so keep it for notes that must disappear (a secret saved by
mistake).

**After substantial work: persist what lasts.** Decisions and their reasons,
fixes and gotchas, new invariants, reusable procedures - see *Writing back* below,
with `--source file:<path>` whenever the note is derived from a file. Not the
chain of thought, not the transcript, not what the code already says.

**Stay out of the way.** Normal operation is silent apart from the one-line
recap. Bring graft to the user only when it is broken (install, daemon, model),
when you suspect data loss or corruption, or when an irreversible action cannot be
decided from the context. The manual skills (`/recall`, `/memoryze`, `/learn`,
`/memory-audit`) remain for the user who wants to drive by hand or debug.

**Opting out of the hooks** (Claude Code plugin): `GRAFT_HOOKS=0` in the
environment (or in `settings.json` `env`) turns both off, `GRAFT_HOOK_PROMPT=0`
only the per-prompt lookup.

## How much to trust a hit

`query` reports `STRONG` / `WEAK` / `MISS`. **`STRONG` means "something close to
your question exists" - it does not mean "this is true today".** Notes were
written against a codebase and a world that may have moved.

So: read the node, then check it against what is in front of you (the code, the
config, the error). If it holds, use it and say where it came from. If it does
not, see *Fixing stale evidence* below.

## Asking well

Query graft the way you query a search engine, not the way you talk to a person.

- **Short.** 3-8 words. `angular signal untracked reactivity`, not
  "why doesn't my Angular component refresh when I change the document?"
- **Nouns over sentences.** Technology, symptom, subject. Drop articles, drop
  "how do I", drop the project's own names unless they are the point.
- **Let `classify` choose the keywords.** `graft classify --title "<your short
  query>"` returns the vocabulary the graph actually uses; feed those back into
  `explore --keyword`. Guessing keywords yourself is how you produce false misses.

## The search loop

```bash
graft classify --title "angular signal reactivity"          # which keywords exist
graft query    "angular signal reactivity"                  # STRONG / WEAK / MISS
graft retrieve "angular signal reactivity" --top-k 5        # 5 ranked candidates
graft explore  "angular signal reactivity" --keyword angular --depth 2 --beam 5
graft get      <hex_id>                                     # full body of a node
```

Run it in that order and stop as soon as you have something usable:

1. `classify` when you are unsure what the graph calls this subject.
2. `query` for the fast yes/no.
3. On `WEAK` or `MISS` that still smells like a hit, `retrieve --top-k 5` or
   `explore ... --beam 5` - five candidates is the sweet spot; more is noise.
4. `get` the ids that look relevant. Titles lie a little; bodies do not.

`/recall <question>` runs this escalation for you and is the normal entry point.

## When to search

Search before: a technical problem, bug or error; a design decision; "how do I /
why does / what's the right way"; anything the user phrases as "we did this
before"; any non-trivial chunk of code or architecture you are about to commit to.

Skip only for mechanical edits (rename, typo, format), file listings, and
questions already answered by code on screen.

## Writing back - the part that is easy to skip

At the end of a piece of work, ask one question: **was everything I just figured
out already in the graph?** If not, insert the missing notes. More than one is
normal - a session that produced a fix, a gotcha and a decision produces three.

Write a note when you have:

- a bug and a non-obvious fix,
- a library / framework / CLI quirk,
- an architectural decision **and the reason it won**,
- a working incantation that was hard to find,
- a standing convention ("from now on we always X").

Do not write: trivia, secrets or tokens, chit-chat, or anything a reader could
derive from the current code or git history.

```bash
graft classify --title "short searchable title"
graft insert --title "short searchable title" \
             --body "what it is, why it matters, what it cost to learn" \
             --keyword k1 --keyword k2
```

The title is the retrieval anchor: phrase it the way you would *search* for it
later, not the way you solved it. `insert` is idempotent - the same
title+body+keywords returns the existing id with `"duplicate": true`.

When a note is derived from a file, add `--source file:<path>` (repeatable): graft
stores the file's project-relative path and fingerprint. `graft sources diff` then
lists the notes whose files changed or disappeared, so you can revalidate them
(fix them as below, or confirm them with `graft sources refresh <hex_id>`).

Use `/memoryze` for 1-5 notes out of the conversation, `/learn` for bulk ingestion
from files outside it, `/learn docs` to take in all of a repository's documentation,
`/learn bootstrap` to build a new project's memory progressively (`graft project status`
says whether it was bootstrapped and which topics are still pending).

## Fixing stale evidence

When a note is contradicted by what you can see now, the graph must be corrected -
a wrong note is worse than a missing one, because it will be retrieved with
confidence.

```bash
graft get    <hex_id>     # 1. read what is there
graft insert --title ... --body ... --keyword ...   # 2. insert the current truth -> <new>
graft maintain resolve --node <hex_id> --action supersede --by <new> --note "<why>"   # 3.
```

Supersede, not "leave it and add another": two contradicting notes about the same
thing is the worst state the graph can be in. The superseded note drops out of
search but stays in the audit log and can be restored. If the note is merely
suspicious and you cannot verify it, mark it instead:
`graft maintain resolve --node <hex_id> --action stale --note "<what you could not confirm>"`.
Use `graft delete` only for what must vanish for good (a secret saved by mistake).

## End-of-turn recap

Every turn in which you touched graft, close with one short line saying how you
used it. Not a section, not a table - a line:

> graft: STRONG hit on "sqlite wal checkpoint" (used), 1 node added for the
> retry-backoff decision.

or, when it gave you nothing:

> graft: MISS on "msgpack nested map"; added 2 nodes so it will not miss again.

This is what makes the memory visible and keeps you honest about maintaining it.

## Profiles - separate graphs

A profile is its own DB and its own daemon. Default is `default`.

```bash
graft profile list | current | add <name> | remove <name>
graft profile export <name> --path <file>
graft profile import --name <name> --file <file> [--force]
eval "$(graft profile set work)"     # bash/zsh/fish
graft profile set work | iex         # PowerShell
GRAFT_PROFILE=work graft query "deployment"   # one-off
```

Resolution: `$GRAFT_PROFILE`, else `default`. No global state file.

## CLI reference

| Goal | Command |
| ---- | ------- |
| Fast STRONG/WEAK/MISS check | `graft query "<text>"` |
| Ranked hybrid results | `graft retrieve "<text>" --top-k 5` |
| Graph walk from keywords | `graft explore "<text>" --keyword K --depth 2 --beam 5` |
| Suggested keywords for a title | `graft classify --title "<text>"` |
| Full node by id | `graft get <hex_id>` |
| Save a note | `graft insert --title T --body B --keyword K` |
| Save with provenance | `graft insert ... --source file:<path>` |
| Notes whose source files changed | `graft sources diff [--changed-only]` |
| Mark a note revalidated | `graft sources refresh <hex_id>` |
| Is any housekeeping due here? | `graft status` |
| Replace an outdated note | `graft maintain resolve --node <old> --action supersede --by <new>` |
| Remove a node | `graft delete <hex_id>` |
| Graph statistics | `graft stats` |
| Hit-rate / usage report | `graft analytics [--since 7d]` |
| Install the skills into your agents | `graft setup` |

Every command prints `{ "status": 0, "result": { ... } }` on success, or
`{ "status": <n>, "error": "...", "result": null }` on failure. Exit codes:
`0` ok, `1` transport failure, `3` handler error.

## When something breaks

| Symptom | Cause | What to do |
| ------- | ----- | ---------- |
| `connect failed` + `auto-start failed` | binary or model missing | re-run the installer; the second line names the cause - surface it verbatim |
| `status: 5` | model file unreadable | check `~/.graft/models/bge-m3.gguf` |
| empty results | the graph has nothing yet | do not invent one - solve, then insert |
| `daemon spawned but socket not ready` | daemon died at startup | read `~/.graft/graftd.log` |
| `profile X is currently in use` | its daemon is running | stop `graftd`, then retry |

## Is it paying off

Run `/memory-audit` now and then. Healthy looks like: `hit_rate >= 0.30` on a
mature graph, more reads than writes, and a small set of nodes carrying most of
the STRONG hits. A bad hit rate usually means bad titles, not bad content -
re-save with the phrasing you would have searched for.
