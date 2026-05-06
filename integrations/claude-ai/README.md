# Claude AI (Desktop) — memgraph MCP

Connects the Claude Desktop app to the local `memgraph` daemon via the MCP server in `../mcp-server/`.

## Prerequisites

1. **Build memgraph**: `cmake --build build` produces `build/memgraph.exe` and `build/memgraphd.exe`.
2. **Start the daemon**:
   ```bash
   ./build/memgraphd --config ./config.example.yaml &
   ```
3. **Install the MCP server** (Python 3.10+):
   ```bash
   cd integrations/mcp-server && pip install -e .
   ```

## Wire it up

Claude Desktop config lives at:

| OS       | Path                                                          |
| -------- | ------------------------------------------------------------- |
| macOS    | `~/Library/Application Support/Claude/claude_desktop_config.json` |
| Windows  | `%APPDATA%\Claude\claude_desktop_config.json`                  |
| Linux    | `~/.config/Claude/claude_desktop_config.json`                  |

Merge the `mcpServers.memgraph` block from `claude_desktop_config.json` in this directory into your existing config (preserving any other `mcpServers` you already have). Adjust the absolute paths to match your machine.

Example minimal config (Windows path conventions):

```json
{
  "mcpServers": {
    "memgraph": {
      "command": "python",
      "args": ["C:/path/to/agent-memory/integrations/mcp-server/server.py"],
      "env": {
        "MEMGRAPH_BIN": "C:/path/to/agent-memory/build/memgraph.exe",
        "MEMGRAPH_SOCKET": "/tmp/memgraph.sock"
      }
    }
  }
}
```

## Verify

Restart Claude Desktop. In a chat, the tools menu should now list:
- `memgraph_query`, `memgraph_retrieve`, `memgraph_explore`
- `memgraph_insert`, `memgraph_classify`, `memgraph_get`, `memgraph_stats`

Try: "Use memgraph_stats to check the memory state".

## Tip — system prompt

Add a Project-level instruction:

> You have access to a long-term memory via the `memgraph_*` tools. Call `memgraph_query` before solving non-trivial problems, and `memgraph_insert` after solving novel ones. See the tool descriptions for details.

## Troubleshooting

- **Tool list empty / no memgraph**: check Claude Desktop logs (`Help > View Logs`). Common: wrong path to `server.py`, Python not on PATH.
- **Tool calls fail with "connect failed"**: `memgraphd` is not running.
- **`mcp` not installed**: in the same Python that Claude is invoking, `pip install mcp`.
