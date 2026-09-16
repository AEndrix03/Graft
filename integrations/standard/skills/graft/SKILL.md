---
name: graft
description: >-
  Persistent graph memory shared across conversations and agents. Graft is a prompter, not a cache: it hands you notes that may be close to the problem in front of you, and a close note is already a win - it gives you a starting point and reminds you what was decided before. Search it before non-trivial work, and write to it whenever the answer you just produced was not already there. Companion skills: `/recall` (search), `/memoryze` (save), `/learn` (bulk ingest), `/memory-audit` (health check). The daemon auto-starts on the first command.
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

Use `/memoryze` for 1-5 notes out of the conversation, `/learn` for bulk ingestion
from files outside it.

## Fixing stale evidence

When a note is contradicted by what you can see now, the graph must be corrected -
a wrong note is worse than a missing one, because it will be retrieved with
confidence.

```bash
graft get    <hex_id>     # 1. read what is there
graft delete <hex_id>     # 2. remove the outdated node
graft insert --title ... --body ... --keyword ...   # 3. insert the current truth
```

Delete + insert, not "leave it and add another": two contradicting notes about the
same thing is the worst state the graph can be in. If the note is merely
suspicious and you cannot verify it, re-save it with an `unsure` keyword and a
body that says what you could not confirm, rather than deleting.

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
