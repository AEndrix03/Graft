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
`AGENTS.md`, `~/.codex/hooks.json`, or `~/.codex/config.toml`.

For repo-level instructions:

```bash
cat integrations/standard/project-snippet.md >> ./AGENTS.md
```

## Allow-listing

Codex may sandbox shell commands. Whitelist the `graft` binary in your Codex
config if you do not want a prompt on every call.
