# Codex - graft integration

Codex can use two layers:

- Skills: user-scoped instructions loaded by Codex.
- `AGENTS.md`: static instructions loaded as model context.
- Hooks: deterministic `UserPromptSubmit`, `PostToolUse`, and `Stop` helpers.

The shared source is `integrations/standard`; this directory is an adapter and
manual-install reference for Codex.

## Install

```bash
graft setup codex
```

Setup installs compatible skills to `~/.codex/skills` only. It does not modify
`AGENTS.md`, `~/.codex/hooks.json`, or `~/.codex/config.toml`. Run `/graft-init` inside Codex to write the usage rule into `AGENTS.md`.

For repo-level instructions:

```bash
# then, inside Codex:  /graft-init
```

## Allow-listing

Codex may sandbox shell commands. Whitelist the `graft` binary in your Codex
config if you do not want a prompt on every call.
