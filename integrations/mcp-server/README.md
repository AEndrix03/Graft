# memgraph-mcp

Bridges MCP-aware clients to `memgraph`. Local/dev stdio remains simple; production remote MCP runs through the OAuth/OIDC gateway.

## Tools

Search/read tools require `memgraph:read` in remote mode:
`memgraph_query`, `memgraph_retrieve`, `memgraph_explore`, `memgraph_classify`, `memgraph_get`, `memgraph_stats`, `memgraph_analytics`.

Write tools require `memgraph:write`:
`memgraph_insert`.

Admin tools require `memgraph:admin`:
`memgraph_delete`, profile add/remove/import/export/merge/list/current.

Stdio mode has no OAuth context and is intended for local development.

## Install

```bash
cd integrations/mcp-server
pip install -e .
# or
uv pip install -e .
```

## Local Stdio

```bash
# Start the daemon locally.
./build/memgraphd --config ./config.example.yaml

# In a client config, point at:
python integrations/mcp-server/server.py
```

The stdio server wraps the `memgraph` CLI and speaks MCP on stdin/stdout.

## Production HTTP Gateway

`oauth_gateway.py` is an ASGI resource server. It mounts MCP streamable HTTP at `/mcp` and proxies authenticated REST calls under `/v1/*` to the local daemon, default `http://127.0.0.1:9977`.

```bash
export MEMGRAPH_OAUTH_ISSUER_URL="https://issuer.example.com"
export MEMGRAPH_OAUTH_RESOURCE_SERVER_URL="https://memgraph.example.com/mcp"
export MEMGRAPH_OAUTH_AUDIENCE="https://memgraph.example.com"
export MEMGRAPH_OAUTH_REQUIRED_SCOPES="memgraph:read"
export MEMGRAPH_UPSTREAM_HTTP="http://127.0.0.1:9977"

uvicorn oauth_gateway:app --host 127.0.0.1 --port 8080
```

Put this behind HTTPS before exposing it publicly. The external OIDC provider handles login, consent, client registration, and token issuance; memgraph only validates access tokens, audience, issuer, expiration, and scopes.

## Environment

| Var | Default | Purpose |
| --- | --- | --- |
| `MEMGRAPH_BIN` | PATH, user install, repo build | Path to `memgraph` CLI for stdio/MCP tools |
| `MEMGRAPH_SOCKET` | CLI default | Daemon socket override read by the CLI |
| `MEMGRAPH_PROFILE` | `default` | Active CLI profile |
| `MEMGRAPH_OAUTH_ISSUER_URL` | required | OIDC issuer that publishes discovery metadata |
| `MEMGRAPH_OAUTH_RESOURCE_SERVER_URL` | required | Public MCP resource URL, usually `https://host/mcp` |
| `MEMGRAPH_OAUTH_AUDIENCE` | required | Expected JWT `aud` |
| `MEMGRAPH_OAUTH_REQUIRED_SCOPES` | `memgraph:read` | Baseline scopes advertised by MCP protected resource metadata |
| `MEMGRAPH_UPSTREAM_HTTP` | `http://127.0.0.1:9977` | Local daemon HTTP upstream for `/v1/*` |
| `MEMGRAPH_OAUTH_JWKS_CACHE_SECONDS` | `300` | JWKS cache lifetime |

## REST Scope Policy

`GET /v1/match`, `/v1/search`, `/v1/explore`, `/v1/classify`, `/v1/nodes/{id}`, and `/v1/view` require `memgraph:read`.

`POST /v1/insert` requires `memgraph:write`.

`DELETE /v1/nodes/{id}` requires `memgraph:admin`.

`GET /v1/healthz` is unauthenticated for liveness checks.
