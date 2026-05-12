# Codex - graft integration

Codex uses two layers:

- `AGENTS.md`: static instructions loaded as model context.
- Hooks: deterministic `UserPromptSubmit`, `PostToolUse`, and `Stop` helpers.

The shared source is `integrations/standard`; this directory is an adapter and
manual-install reference for Codex.

## Install

```bash
graft setup codex
```

Setup installs compatible skills to `~/.codex/skills`, hooks to
`~/.codex/hooks/graft`, writes `~/.codex/hooks.json`, enables
`[features].hooks = true`, and prints project instructions for `AGENTS.md`.

For repo-level instructions:

```bash
cat integrations/standard/project-snippet.md >> ./AGENTS.md
```

## Allow-listing

Codex may sandbox shell commands. Whitelist the `graft` binary in your Codex
config if you do not want a prompt on every call.
