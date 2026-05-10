# Gemini CLI — graft integration

[Gemini CLI](https://github.com/google-gemini/gemini-cli) reads `GEMINI.md` files (project- or user-level) as long-term context.

## Install

```bash
# Project-local
cp integrations/gemini-cli/GEMINI.md ./GEMINI.md

# User-global (~/.gemini/GEMINI.md)
mkdir -p ~/.gemini
cp integrations/gemini-cli/GEMINI.md ~/.gemini/GEMINI.md
```

Multiple `GEMINI.md` files are concatenated with project-local overriding global.

## Daemon

```bash
./build/graftd --config ./config.example.yaml &
export GRAFT_SOCKET=/tmp/graft.sock
```

## Tool allow-list

In `~/.gemini/settings.json` add:

```json
{
  "tool": {
    "shell": {
      "allowList": ["graft"]
    }
  }
}
```

This skips per-command confirmation for `graft` invocations.

## Tip: build a custom slash command

Gemini CLI supports `.gemini/commands/*.toml`. Example `.gemini/commands/recall.toml`:

```toml
description = "Search graft for prior knowledge."
prompt = "Run `graft query \"$1\"` and summarize the response."
```

Then `/recall "spring boot validation"` triggers it.
