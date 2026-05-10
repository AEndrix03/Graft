# graft-mcp

Bridges MCP-aware clients to `graft`. Local/dev stdio remains simple; production remote MCP runs through the OAuth/OIDC gateway.

## Tools

Search/read tools require `graft:read` in remote mode:
`graft_query`, `graft_retrieve`, `graft_explore`, `graft_classify`, `graft_get`, `graft_stats`, `graft_analytics`.

Write tools require `graft:write`:
`graft_insert`.

Admin tools require `graft:admin`:
`graft_delete`, profile add/remove/import/export/merge/list/current.

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
./build/graftd --config ./config.example.yaml

# In a client config, point at:
python integrations/mcp-server/server.py
```

The stdio server wraps the `graft` CLI and speaks MCP on stdin/stdout.

## Production HTTP Gateway

`oauth_gateway.py` is an ASGI resource server. It mounts MCP streamable HTTP at `/mcp` and proxies authenticated REST calls under `/v1/*` to the local daemon, default `http://127.0.0.1:9977`.

```bash
export GRAFT_OAUTH_ISSUER_URL="https://issuer.example.com"
export GRAFT_OAUTH_RESOURCE_SERVER_URL="https://graft.example.com/mcp"
export GRAFT_OAUTH_AUDIENCE="https://graft.example.com"
export GRAFT_OAUTH_REQUIRED_SCOPES="graft:read"
export GRAFT_UPSTREAM_HTTP="http://127.0.0.1:9977"

uvicorn oauth_gateway:app --host 127.0.0.1 --port 8080
```

Put this behind HTTPS before exposing it publicly. The external OIDC provider handles login, consent, client registration, and token issuance; graft only validates access tokens, audience, issuer, expiration, and scopes.

## Environment

| Var | Default | Purpose |
| --- | --- | --- |
| `GRAFT_BIN` | PATH, user install, repo build | Path to `graft` CLI for stdio/MCP tools |
| `GRAFT_SOCKET` | CLI default | Daemon socket override read by the CLI |
| `GRAFT_PROFILE` | `default` | Active CLI profile |
| `GRAFT_OAUTH_ISSUER_URL` | required | OIDC issuer that publishes discovery metadata |
| `GRAFT_OAUTH_RESOURCE_SERVER_URL` | required | Public MCP resource URL, usually `https://host/mcp` |
| `GRAFT_OAUTH_AUDIENCE` | required | Expected JWT `aud` |
| `GRAFT_OAUTH_REQUIRED_SCOPES` | `graft:read` | Baseline scopes advertised by MCP protected resource metadata |
| `GRAFT_UPSTREAM_HTTP` | `http://127.0.0.1:9977` | Local daemon HTTP upstream for `/v1/*` |
| `GRAFT_OAUTH_JWKS_CACHE_SECONDS` | `300` | JWKS cache lifetime |

## REST Scope Policy

`GET /v1/match`, `/v1/search`, `/v1/explore`, `/v1/classify`, `/v1/nodes/{id}`, and `/v1/view` require `graft:read`.

`POST /v1/insert` requires `graft:write`.

`DELETE /v1/nodes/{id}` requires `graft:admin`.

`GET /v1/healthz` is unauthenticated for liveness checks.
