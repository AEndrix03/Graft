# ChatGPT - memgraph MCP

Connects ChatGPT to `memgraph` via MCP.

## Local Stdio

If your ChatGPT client supports local stdio MCP servers in developer mode, use `mcp_config.json` and adjust the paths. This is for local development.

## Production Connector

For remote ChatGPT connectors, run the OAuth/OIDC gateway from `../mcp-server/` and configure ChatGPT with the public HTTPS URL:

```text
https://memgraph.example.com/mcp
```

The gateway is an OAuth resource server. Your external OIDC provider issues access tokens; memgraph validates issuer, audience, expiration, and scopes. Required scopes:

- `memgraph:read` for search, retrieve, explore, classify, get, stats, analytics
- `memgraph:write` for insert
- `memgraph:admin` for delete and profile administration

Gateway command:

```bash
cd integrations/mcp-server
export MEMGRAPH_OAUTH_ISSUER_URL="https://issuer.example.com"
export MEMGRAPH_OAUTH_RESOURCE_SERVER_URL="https://memgraph.example.com/mcp"
export MEMGRAPH_OAUTH_AUDIENCE="https://memgraph.example.com"
uvicorn oauth_gateway:app --host 127.0.0.1 --port 8080
```

Expose the gateway only behind HTTPS. Keep `memgraphd` bound to `127.0.0.1`.

## Tool List

Search: `memgraph_query`, `memgraph_retrieve`, `memgraph_explore`
Save: `memgraph_classify`, `memgraph_insert`
Maintain: `memgraph_get`, `memgraph_delete`, `memgraph_stats`, `memgraph_analytics`
Profiles: `memgraph_profile_list`, `memgraph_profile_current`, `memgraph_profile_add`, `memgraph_profile_remove`, `memgraph_profile_export`, `memgraph_profile_import`, `memgraph_profile_merge`

Each search/save/maintenance tool accepts optional `profile=<name>` to target a tenant per call.

## Custom GPT Instructions Snippet

Paste in the GPT's instructions:

> You have access to a long-term memory graph via `memgraph_*` MCP tools. Before solving any non-trivial technical problem, call `memgraph_query` with a concise restatement of the user's request. If you get STRONG hit, reuse the answer; on MISS, proceed and consider `memgraph_insert` after solving, using `memgraph_classify` to align keywords. When you find an obsolete or wrong node, `memgraph_delete` it; to modify a node, fetch with `memgraph_get`, delete, then re-insert with corrected fields.
