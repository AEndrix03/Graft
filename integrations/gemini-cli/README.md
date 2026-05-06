# Gemini CLI — memgraph integration

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
./build/memgraphd --config ./config.example.yaml &
export MEMGRAPH_SOCKET=/tmp/memgraph.sock
```

## Tool allow-list

In `~/.gemini/settings.json` add:

```json
{
  "tool": {
    "shell": {
      "allowList": ["memgraph"]
    }
  }
}
```

This skips per-command confirmation for `memgraph` invocations.

## Tip: build a custom slash command

Gemini CLI supports `.gemini/commands/*.toml`. Example `.gemini/commands/recall.toml`:

```toml
description = "Search memgraph for prior knowledge."
prompt = "Run `memgraph query \"$1\"` and summarize the response."
```

Then `/recall "spring boot validation"` triggers it.
