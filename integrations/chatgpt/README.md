# ChatGPT - graft MCP

Connects ChatGPT to `graft` via MCP.

## Local Stdio

If your ChatGPT client supports local stdio MCP servers in developer mode, use `mcp_config.json` and adjust the paths. This is for local development.

## Production Connector

For remote ChatGPT connectors, run the OAuth/OIDC gateway from `../mcp-server/` and configure ChatGPT with the public HTTPS URL:

```text
https://graft.example.com/mcp
```

The gateway is an OAuth resource server. Your external OIDC provider issues access tokens; graft validates issuer, audience, expiration, and scopes. Required scopes:

- `graft:read` for search, retrieve, explore, classify, get, stats, analytics
- `graft:write` for insert
- `graft:admin` for delete and profile administration

Gateway command:

```bash
cd integrations/mcp-server
export GRAFT_OAUTH_ISSUER_URL="https://issuer.example.com"
export GRAFT_OAUTH_RESOURCE_SERVER_URL="https://graft.example.com/mcp"
export GRAFT_OAUTH_AUDIENCE="https://graft.example.com"
uvicorn oauth_gateway:app --host 127.0.0.1 --port 8080
```

Expose the gateway only behind HTTPS. Keep `graftd` bound to `127.0.0.1`.

## Tool List

Search: `graft_query`, `graft_retrieve`, `graft_explore`
Save: `graft_classify`, `graft_insert`
Maintain: `graft_get`, `graft_delete`, `graft_stats`, `graft_analytics`
Profiles: `graft_profile_list`, `graft_profile_current`, `graft_profile_add`, `graft_profile_remove`, `graft_profile_export`, `graft_profile_import`, `graft_profile_merge`

Each search/save/maintenance tool accepts optional `profile=<name>` to target a tenant per call.

## Custom GPT Instructions Snippet

Paste in the GPT's instructions:

> You have access to a long-term memory graph via `graft_*` MCP tools. Before solving any non-trivial technical problem, call `graft_query` with a concise restatement of the user's request. If you get STRONG hit, reuse the answer; on MISS, proceed and consider `graft_insert` after solving, using `graft_classify` to align keywords. When you find an obsolete or wrong node, `graft_delete` it; to modify a node, fetch with `graft_get`, delete, then re-insert with corrected fields.
