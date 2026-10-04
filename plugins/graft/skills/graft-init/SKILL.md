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
prompter, not a cache: a close note is already a win. You run it - the user should
never have to think about it. The `graft` skill has the full lifecycle; these
rules always apply.

**Housekeeping, once per session in a project**: `graft status` (cheap, never
starts the daemon; a `graft:` session hint, when present, already carries it)
and act on its `next` list, bounded and never blocking the user's task:
`bootstrap` - one `/learn bootstrap` pass, task area first, in the background if
you can; `apply-safe` - run it; `resolve` / `refresh-sources` / `scan` - one
small batch, the `memory-audit` way. Empty `next` - nothing to do.

**Before non-trivial work** (a bug, an error, a design decision, a "how do I", a
substantial piece of code) search first: the `recall` skill, or
`graft query "<3-8 words>"`, then `graft retrieve --top-k 5`, `graft get <id>`.
`STRONG` means "something close exists", not "this is correct": check it.

**A note contradicted by what you see is repaired now**: insert the corrected
note, then `graft maintain resolve --node <old> --action supersede --by <new>`
(`--action stale` if you cannot write the correction). Never leave two
contradicting notes.

**After substantial work, close the gap**: insert what the graph did not have -
the fix, the gotcha, the decision and its reason, a reusable procedure (the
`memoryze` skill), with `--source file:<path>` when it comes from a file. Never
save secrets, transcripts, or what the code or git history already says.

**Stay invisible.** Never ask the user about graft in the normal path; raise it
only when it is broken, data loss is suspected, or an irreversible action cannot
be decided from context. One recap line in a turn where graft gave or took
something is enough.

Skip all of this for mechanical edits and questions answered by code on screen.
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
2. One sentence: from now on the agent runs graft by itself - bootstrap,
   search, repair, save and maintenance, bounded and without asking - and
   nothing else is needed from the user.

Do not paste the rule back to the user.

## Failure modes

- Cancel on the question - stop, nothing written.
- A directory cannot be created - report the path and the error verbatim, do not retry blindly.
- A target file is read-only - report it and stop; do not change permissions without asking.
- `graft` is not installed and the user cancels step 0 - stop, nothing written;
  the rule is useless until the CLI answers.
- The installer fails - report its last lines verbatim and stop.
