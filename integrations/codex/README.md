# Codex - graft integration

Codex uses two layers:

- Skills: user-scoped instructions loaded by Codex.
- `AGENTS.md`: the usage rule `graft-init` writes, loaded as model context.

The rule is a zero-touch lifecycle: once per session in a project the agent runs
`graft status` (one cheap call that never starts the daemon) and does the bounded
housekeeping its `next` list asks for (a progressive `/learn bootstrap` pass on a
new project, `apply-safe`, a few maintenance candidates), searches before
non-trivial work, supersedes notes the code contradicts, saves what it learned,
and does not ask the user about graft in the normal path. No hooks ship for Codex
(the per-prompt lookup hook is Claude Code only).

The shared source is the plugin in `plugins/graft`; this directory is an adapter
and manual-install reference for Codex.

## Install

From the marketplace (recommended):

```bash
codex plugin marketplace add AEndrix03/Graft
codex plugin add graft@graft
```

Then, inside Codex, run the `graft-init` skill (or ask *"set up graft"*): it
installs the `graft` CLI if it is missing, after asking, and writes the usage
rule. Plugin skills are namespaced (`graft:recall`, `graft:memoryze`, ...).

Without the marketplace:

```bash
graft setup codex
```

Setup installs compatible skills to `~/.codex/skills` only, and skips Codex when
the graft plugin is already installed. It does not modify
`AGENTS.md`, `~/.codex/hooks.json`, or `~/.codex/config.toml`. Run `/graft-init` inside Codex to write the usage rule into `AGENTS.md`.

For repo-level instructions:

```bash
# then, inside Codex:  /graft-init
```

## Allow-listing

Codex may sandbox shell commands. Whitelist the `graft` binary in your Codex
config if you do not want a prompt on every call.
