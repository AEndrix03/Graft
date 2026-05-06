# memgraph-mcp — MCP server

Bridges MCP-aware chat clients (Claude AI, ChatGPT) to the local `memgraph` daemon by wrapping the `memgraph` CLI via subprocess.

## Tools exposed

- `memgraph_query(text, signals_only=False)`
- `memgraph_retrieve(text, top_k=10)`
- `memgraph_explore(text, keywords=[], depth=3, beam=4)`
- `memgraph_insert(summary, detail, keywords=[])`
- `memgraph_classify(summary)`
- `memgraph_get(id_hex)`
- `memgraph_stats()`

## Install

```bash
cd integrations/mcp-server
pip install -e .
# or, with uv:
uv pip install -e .
```

## Run (manual smoke test)

```bash
# 1. Start the memgraph daemon in one terminal
./build/memgraphd --config ./config.example.yaml &

# 2. Run the MCP server (waits on stdio)
python integrations/mcp-server/server.py
```

The server speaks MCP over stdio. To test interactively, use [`mcp-inspector`](https://modelcontextprotocol.io/docs/tools/inspector).

## Environment

| Var               | Default              | Purpose                                  |
| ----------------- | -------------------- | ---------------------------------------- |
| `MEMGRAPH_BIN`    | (search PATH, build) | Path to `memgraph` CLI binary            |
| `MEMGRAPH_SOCKET` | `/tmp/memgraph.sock` | Daemon socket (read by the CLI itself)   |

## Wiring into clients

See:
- `../claude-ai/README.md` for Claude Desktop
- `../chatgpt/README.md` for ChatGPT custom MCP

## Notes

- Subprocess timeout: 60s. Tune in `server.py:_run` if you have heavy embeds / large k.
- The CLI exit codes: `0`=ok, `3`=remote handler returned status != 0 (parsed and forwarded). Other codes = transport/process failure (raised as RuntimeError, surfaces as MCP tool error).
