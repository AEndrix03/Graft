# Claude AI (Desktop) — graft MCP

Connects the Claude Desktop app to the local `graft` daemon via the MCP server in `../mcp-server/`.

## Prerequisites

1. **Install graft end-to-end** (one command, idempotent):
   ```bash
   pwsh scripts/install.ps1     # Windows (auto-bootstraps MSYS2 if needed)
   bash  scripts/install.sh     # Linux / macOS / inside MSYS2
   ```
   This puts `graft[.exe]` at `~/.graft/bin/` and adds it to user PATH. The daemon auto-starts on first command, no manual launch needed.

2. **Install the Python MCP SDK**:
   ```bash
   pip install mcp
   ```
   Use the same Python that Claude Desktop will spawn (see config below).

## Wire it up

Claude Desktop config lives at:

| OS       | Path                                                              |
| -------- | ----------------------------------------------------------------- |
| macOS    | `~/Library/Application Support/Claude/claude_desktop_config.json` |
| Windows  | `%APPDATA%\Claude\claude_desktop_config.json`                     |
| Linux    | `~/.config/Claude/claude_desktop_config.json`                     |

**Merge** the `mcpServers.graft` block from `claude_desktop_config.json` in this directory into your existing config (preserve any other `mcpServers` you already have). Adjust the absolute paths to your machine.

Reference (Windows):

```json
{
  "mcpServers": {
    "graft": {
      "command": "python",
      "args": ["C:/personal/Graft/integrations/mcp-server/server.py"],
      "env": {
        "GRAFT_BIN": "C:/Users/<user>/.graft/bin/graft.exe"
      }
    }
  }
}
```

If `python` isn't on Claude Desktop's PATH, use the absolute python.exe path.

## Verify

Restart Claude Desktop. In a chat, the tools menu should expose 16 tools:

**Search** — `graft_query`, `graft_retrieve`, `graft_explore`
**Save**   — `graft_classify`, `graft_insert`
**Maintain** — `graft_get`, `graft_delete`, `graft_stats`, `graft_analytics`
**Profiles** — `graft_profile_list`, `graft_profile_current`, `graft_profile_add`, `graft_profile_remove`, `graft_profile_export`, `graft_profile_import`, `graft_profile_merge`

All search/save/maintenance tools accept an optional `profile=<name>` arg to target a specific tenant per call.

Quick smoke: ask Claude "use graft_stats" — should return node/edge/keyword counts.

## System prompt suggestion

Add a Project-level instruction:

> You have access to a long-term memory graph via the `graft_*` tools. **ALWAYS** call `graft_query` BEFORE solving non-trivial problems. **AFTER** solving a novel problem, call `graft_classify` then `graft_insert` to save the answer. To remove obsolete or wrong nodes, use `graft_delete`. To "modify" a node, fetch (`graft_get`), delete it, then re-insert with corrected fields — the system rebuilds embeddings and edges automatically.

## Troubleshooting

- **Tool list empty**: check Claude Desktop logs (`Help > View Logs`). Common causes:
  - Wrong path to `server.py` in the config.
  - `python` not on Claude Desktop's spawn PATH → use absolute path.
  - `mcp` package not installed in the Python being invoked.
- **Tool calls fail with `connect failed: …`**: the per-profile daemon couldn't start. Check `~/.graft/graftd.log` and verify the binaries / model are in place.
- **`profile X is currently in use`**: kill any stray daemon (`Stop-Process graftd` on Windows, `pkill graftd` on POSIX) and retry.
- **Wrong profile being used**: pass `profile=<name>` explicitly to the tool, or set the `GRAFT_PROFILE` env in the config block.
