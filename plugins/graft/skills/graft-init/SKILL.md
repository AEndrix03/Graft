---
name: graft-init
description: >-
  One-shot wiring of graft into this agent. Installs the graft CLI first if it is missing (after asking), then asks a single question - global or project - then writes the graft usage rule into the right instruction file (CLAUDE.md or AGENTS.md) and, on Claude Code, a rule file under .claude/rules/graft.md that the instruction file imports. Triggered by `/graft-init`, "configura graft", "set up graft here", "enable graft globally". Idempotent: re-running replaces the previous block in place, it never duplicates or strips anything else.
---

# graft-init - wire graft into this agent

The skills reach the agent either from the graft plugin (marketplace) or from
`graft setup`. This skill does the rest: it makes sure the `graft` CLI is
installed, then tells the agent to actually *use* the skills, every session,
without being asked. One question, then it writes the files.

## Step 0 - make sure graft is installed

Run `graft --version`. If that fails, also try the default install location
directly - right after an install the PATH of this session is stale, on Windows
especially: `~/.graft/bin/graft --version` (`$env:USERPROFILE\.graft\bin\graft.exe`
on Windows). Use whichever path answers for every `graft` command below.

**Not installed.** Ask before downloading anything (`AskUserQuestion`):

```
question: "graft is not installed. Install it now?"
header:   "Install"
options:
  - { label: "Install (recommended)", description: "Prebuilt, checksum-verified binaries into ~/.graft, plus the embedding model (~600 MB, downloaded once). No account, no API key." }
  - { label: "Cancel",                description: "Install nothing. graft-init cannot continue without the CLI." }
```

On **Install**, run the official installer, nothing else:

- Linux: `curl -fsSL https://raw.githubusercontent.com/AEndrix03/Graft/master/install.sh | sh`
- macOS: `brew tap AEndrix03/graft https://github.com/AEndrix03/Graft.git && brew install graft`
- Windows: `powershell -NoProfile -ExecutionPolicy Bypass -Command "irm https://raw.githubusercontent.com/AEndrix03/Graft/master/install.ps1 | iex"`

The model download takes minutes: give the command a long timeout (10 minutes)
or run it in the background and wait for it. If it fails, show the last lines of
its output and stop - do not retry with other flags or try to build from source.

**Installed but older than this plugin.** When the skills came from the plugin,
its manifest carries a `version` (`.claude-plugin/plugin.json` or
`.codex-plugin/plugin.json` in the plugin root, two levels above this skill's folder). If `graft --version`
is lower, say so in one line and offer `graft upgrade --yes`. Do not upgrade
without a yes.

**Duplicate skills.** If the skills came from the plugin, check for copies left by
an older `graft setup`: `graft`, `graft-init`, `recall`, `memoryze`, `learn` and
`memory-audit` under `~/.claude/skills` or `~/.codex/skills`, each with a
`SKILL.md` whose `name:` matches its folder. They make every skill appear twice
(`/recall` and `/graft:recall`). List what you found and offer to delete exactly
those folders; delete nothing else and nothing without a yes.

## Step 1 - ask the one question that matters

Use `AskUserQuestion`, `multiSelect: false`:

```
question: "Where should the graft rule live?"
header:   "Scope"
options:
  - { label: "Global (recommended)", description: "Every project on this machine. The graph is shared anyway, so this is normally what you want." }
  - { label: "This project only",    description: "Only this repo. Use it when the team should get the rule from version control, or when you are trying graft out." }
  - { label: "Cancel",               description: "Change nothing." }
```

On **Cancel**: reply `Cancelled, nothing changed.` and stop.

Ask nothing else. Do not ask about caching, retrieval frequency or save policy -
the rule below is the answer to all three, and asking makes the user choose
between options they have no basis to choose between yet.

## Step 2 - resolve the targets

Detect the agent from the directories that exist, and pick the paths:

| Agent | Global | Project |
| ----- | ------ | ------- |
| Claude Code (`~/.claude`) | `~/.claude/CLAUDE.md` + `~/.claude/rules/graft.md` | `./CLAUDE.md` + `./.claude/rules/graft.md` |
| Codex (`~/.codex`) | `~/.codex/AGENTS.md` | `./AGENTS.md` |
| OpenCode (`~/.config/opencode`) | `~/.config/opencode/AGENTS.md` | `./AGENTS.md` |
| anything else | `~/AGENTS.md` | `./AGENTS.md` |

On Windows, `~` is `$env:USERPROFILE`. Create parent directories and missing
files as needed. If several agents are present, write for all of them - it costs
nothing and the user does not have to remember which one they are in today.

## Step 3 - write the rule

**Claude Code**: write the full rule to the `rules/graft.md` path, and put this
in the CLAUDE.md so it is loaded even where the rules directory is not scanned
automatically:

```markdown
<!-- graft:start -->
## graft - persistent memory (configured by /graft-init)

@.claude/rules/graft.md
<!-- graft:end -->
```

Use the path that matches the scope you resolved in step 2 (`~/.claude/rules/graft.md`
for global, `.claude/rules/graft.md` for project-local).

**Every other agent**: write the full rule text directly between the markers in
the AGENTS.md, with no import line.

The rule text, verbatim:

```markdown
# graft - use the memory graph

You have a persistent memory graph (`graft`) shared across every session. It is a
prompter, not a cache: it hands you notes that may be close to the problem, and a
close note is already a win - it shortens the reasoning and tells you what was
decided before. You are also the one who maintains it.

**Before non-trivial work** - a bug, an error, a design decision, a "how do I",
anything the user says you have seen before, any substantial piece of code you
are about to write - search the graph first. Use the `recall` skill
(`/graft:recall` when graft came from the plugin, `/recall` otherwise), or the
raw loop: `graft classify --title "<short query>"` to find the vocabulary, then
`graft query`, then `graft retrieve --top-k 5` or `graft explore --beam 5`, then
`graft get <id>` on whatever looks relevant.

**Query it like a search engine**: 3-8 words, nouns and technologies, no
sentences. Long questions produce false misses.

**`STRONG` means "something close exists", not "this is correct"**. Read the node
and check it against the code in front of you before you rely on it.

**When a note is contradicted by current reality**, fix the graph:
`graft get <id>`, `graft delete <id>`, then `graft insert` the corrected note.
Never leave two contradicting notes about the same thing.

**At the end of the work, close the gap.** If what you just figured out was not
in the graph, insert it - often more than one node: the fix, the gotcha, the
decision and its reason. A miss today is only a waste if you leave it a miss.
Use the `memoryze` skill for notes from the conversation, `learn` for bulk
ingestion from files. Never save secrets, tokens, or anything derivable from the code.

**End every turn in which you touched graft with a one-line recap** of how you
used it - hits, what you took from them, what you added. One line, not a section.

Skip all of this only for mechanical edits (rename, typo, format) and questions
fully answered by code already on screen.
```

## Step 4 - idempotency

Read each target file first.

- If it already contains `<!-- graft:start -->` ... `<!-- graft:end -->`, replace
  exactly that span, markers included. Touch nothing else.
- Otherwise append the block, separated from the existing content by one blank line.
- Overwrite `rules/graft.md` wholesale - it is entirely ours.

Never duplicate the block, never remove content outside the markers.

## Step 5 - report

Two lines, no more:

1. `Wired graft into <path>` (one line per file written).
2. One sentence: search before non-trivial work, insert what was missing at the
   end, recap in one line per turn.

Do not paste the rule back to the user.

## Failure modes

- Cancel on the question - stop, nothing written.
- A directory cannot be created - report the path and the error verbatim, do not retry blindly.
- A target file is read-only - report it and stop; do not change permissions without asking.
- `graft` is not installed and the user cancels step 0 - stop, nothing written;
  the rule is useless until the CLI answers.
- The installer fails - report its last lines verbatim and stop.
