# Claude AI (Desktop) — memgraph MCP

Connects the Claude Desktop app to the local `memgraph` daemon via the MCP server in `../mcp-server/`.

## Prerequisites

1. **Install memgraph end-to-end** (one command, idempotent):
   ```bash
   pwsh scripts/install.ps1     # Windows (auto-bootstraps MSYS2 if needed)
   bash  scripts/install.sh     # Linux / macOS / inside MSYS2
   ```
   This puts `memgraph[.exe]` at `~/.lmemorygraph/bin/` and adds it to user PATH. The daemon auto-starts on first command, no manual launch needed.

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

**Merge** the `mcpServers.memgraph` block from `claude_desktop_config.json` in this directory into your existing config (preserve any other `mcpServers` you already have). Adjust the absolute paths to your machine.

Reference (Windows):

```json
{
  "mcpServers": {
    "memgraph": {
      "command": "python",
      "args": ["C:/personal/LMemoryGraph/integrations/mcp-server/server.py"],
      "env": {
        "MEMGRAPH_BIN": "C:/Users/<user>/.lmemorygraph/bin/memgraph.exe"
      }
    }
  }
}
```

If `python` isn't on Claude Desktop's PATH, use the absolute python.exe path.

## Verify

Restart Claude Desktop. In a chat, the tools menu should expose 16 tools:

**Search** — `memgraph_query`, `memgraph_retrieve`, `memgraph_explore`
**Save**   — `memgraph_classify`, `memgraph_insert`
**Maintain** — `memgraph_get`, `memgraph_delete`, `memgraph_stats`, `memgraph_analytics`
**Profiles** — `memgraph_profile_list`, `memgraph_profile_current`, `memgraph_profile_add`, `memgraph_profile_remove`, `memgraph_profile_export`, `memgraph_profile_import`, `memgraph_profile_merge`

All search/save/maintenance tools accept an optional `profile=<name>` arg to target a specific tenant per call.

Quick smoke: ask Claude "use memgraph_stats" — should return node/edge/keyword counts.

## System prompt suggestion

Add a Project-level instruction:

> You have access to a long-term memory graph via the `memgraph_*` tools. **ALWAYS** call `memgraph_query` BEFORE solving non-trivial problems. **AFTER** solving a novel problem, call `memgraph_classify` then `memgraph_insert` to save the answer. To remove obsolete or wrong nodes, use `memgraph_delete`. To "modify" a node, fetch (`memgraph_get`), delete it, then re-insert with corrected fields — the system rebuilds embeddings and edges automatically.

## Troubleshooting

- **Tool list empty**: check Claude Desktop logs (`Help > View Logs`). Common causes:
  - Wrong path to `server.py` in the config.
  - `python` not on Claude Desktop's spawn PATH → use absolute path.
  - `mcp` package not installed in the Python being invoked.
- **Tool calls fail with `connect failed: …`**: the per-profile daemon couldn't start. Check `~/.lmemorygraph/memgraphd.log` and verify the binaries / model are in place.
- **`profile X is currently in use`**: kill any stray daemon (`Stop-Process memgraphd` on Windows, `pkill memgraphd` on POSIX) and retry.
- **Wrong profile being used**: pass `profile=<name>` explicitly to the tool, or set the `MEMGRAPH_PROFILE` env in the config block.
