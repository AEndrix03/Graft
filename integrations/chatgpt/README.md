# ChatGPT — memgraph MCP

Connects ChatGPT to the local `memgraph` daemon via MCP.

## Caveat

ChatGPT MCP support is delivered via the **Connectors** feature (developer mode required at the time of writing). It supports **remote MCP servers (HTTP/SSE)** more cleanly than local stdio servers. Two integration paths:

### Option A — Local stdio (developer mode)

If your ChatGPT client supports local stdio MCP servers (developer setting "MCP servers"), use the config in `mcp_config.json` (adjust paths). The schema mirrors Claude Desktop's `claude_desktop_config.json`.

### Option B — HTTP shim (recommended for ChatGPT custom GPTs / Connectors)

Wrap `server.py` behind an HTTP server (e.g. via `mcp.server.streamable_http`) and expose it on a public/tunnelled URL. Then in ChatGPT:

1. Settings → Connectors → "Add connector" → MCP.
2. URL: your tunnel (e.g. `https://your-ngrok.app/mcp`).
3. Auth: bearer token if you put one in front.

Run the HTTP shim:
```bash
# minimal: pip install mcp[server]
python - <<'EOF'
from mcp.server.streamable_http import StreamableHTTPServerSession
import server as srv

# the shim depends on your `mcp` SDK version; consult MCP docs.
# This block is a placeholder pointing at the canonical pattern.
EOF
```

The exact streamable-http snippet evolves with the MCP SDK; see <https://modelcontextprotocol.io>.

## Tool list

Same 16 tools as Claude AI (Desktop):

**Search**: `memgraph_query`, `memgraph_retrieve`, `memgraph_explore`
**Save**: `memgraph_classify`, `memgraph_insert`
**Maintain**: `memgraph_get`, `memgraph_delete`, `memgraph_stats`, `memgraph_analytics`
**Profiles**: `memgraph_profile_list`, `memgraph_profile_current`, `memgraph_profile_add`, `memgraph_profile_remove`, `memgraph_profile_export`, `memgraph_profile_import`, `memgraph_profile_merge`

Each search/save/maintenance tool accepts an optional `profile=<name>` to target a tenant per call.

## Custom GPT instructions snippet

Paste in the GPT's "Instructions":

> You have access to a long-term memory graph via `memgraph_*` MCP tools. Before solving any non-trivial technical problem, call `memgraph_query` with a concise restatement of the user's request. If you get STRONG hit, reuse the answer; on MISS, proceed and consider `memgraph_insert` after solving (with `memgraph_classify` to align keywords). When you find an obsolete or wrong node, `memgraph_delete` it; to "modify" a node, fetch with `memgraph_get`, delete, then re-insert with corrected fields — the system rebuilds embeddings and edges automatically.

## Troubleshooting

Same issues as Claude Desktop apply (paths, Python on PATH, daemon running). For HTTP shim path issues, verify the public URL responds to GET / with a non-empty MCP handshake.
