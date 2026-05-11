# HTTP REST API

Graft's primary surface is the unix socket. The HTTP layer is a **thin transport** in front of the same dispatcher — you get the same op handlers, encoded as JSON, on a bindable TCP port. It powers the browser 3D viewer and is what microservice consumers talk to.

**Off by default.** Enable in `config.yaml`:

```yaml
http:
  enabled: true
  bind:    "127.0.0.1"
  port:    9977
  # ... per-endpoint enables
  endpoint_match:    true
  endpoint_search:   true
  endpoint_explore:  true
  endpoint_classify: true
  endpoint_insert:   true
  endpoint_delete:   false      # sensitive: kept off by default
  endpoint_view:     true
  viewer_path:       "viewer/dist"
```

> The full single-file legacy reference is preserved at [`../HTTP-API.md`](../HTTP-API.md). This page is the same content reorganised inside the new docs tree.

---

## The route table

| Method   | Path                  | Default | Description |
| -------- | --------------------- | ------- | ----------- |
| `GET`    | `/v1/healthz`         | always  | Liveness probe. No auth. |
| `GET`    | `/v1/match`           | on      | Cache lookup with multi-signal gating (same as `graft query`). |
| `GET`    | `/v1/search`          | on      | Hybrid top-k via RRF (same as `graft retrieve`). |
| `GET`    | `/v1/explore`         | on      | Beam-search graph walk (same as `graft explore`). |
| `GET`    | `/v1/classify`        | on      | Suggest keywords for a draft title. |
| `POST`   | `/v1/insert`          | on      | Save a node, with optional atomic supersession. |
| `GET`    | `/v1/nodes/{id_hex}`  | on      | Fetch a single node. |
| `DELETE` | `/v1/nodes/{id_hex}`  | **off** | Hard-delete by id. Cascades. |
| `GET`    | `/v1/view`            | on      | Full graph dump for the viewer. |
| `GET`    | `/`                   | always  | Static SPA bundle from `http.viewer_path`. |

Each `/v1/*` endpoint has its own enable flag (`endpoint_match`, `endpoint_search`, etc.). Disabled endpoints return `404`.

## Local-first defaults

- `bind: "127.0.0.1"` — local-only by default. The daemon **does not** bind on `0.0.0.0` unless you flip it explicitly.
- `endpoint_delete: false` — hard delete via HTTP is opt-in. The CLI is the trusted destructive surface.
- **No HTTPS layer in the daemon.** Public exposure should run through `integrations/mcp-server/oauth_gateway.py` behind your reverse proxy of choice (Caddy, nginx, Traefik). Keep `graftd` bound to `127.0.0.1`.

## Response envelope

All `/v1/*` endpoints return the same JSON shape:

```json
{ "status": 0, "result": { ... }, "error": null }
```

- `status: 0` → success. Non-zero is a daemon error code (`mg_err_t`).
- `result` carries the per-endpoint payload.
- `error` is a human-readable string when `status != 0`.

HTTP status codes:

| Code | When |
| ---- | ---- |
| 200  | success |
| 201  | `POST /v1/insert` created a new node |
| 204  | `DELETE /v1/nodes/{id}` succeeded (no body) |
| 400  | malformed query string or JSON body |
| 401  | returned by the OAuth gateway for missing / malformed / expired tokens |
| 404  | unknown route, disabled endpoint, or `id_hex` not found |
| 500  | daemon error (look at stderr / `~/.graft/memgraphd.err.log`) |

`Connection: close` is sent on every response. The HTTP layer is a local-first inspection surface — for heavy programmatic access prefer the unix socket. (The MCP gateway in front of HTTP handles upstream pooling for you.)

---

## Endpoints

### `GET /v1/healthz`

```json
{ "status": "ok", "service": "memgraphd" }
```

Always served, never auth-gated. Use for container probes.

### `GET /v1/match?text=...&signals_only=false`

Cache lookup. Embeds the input, runs vector top-10, runs the verify pipeline, picks the strongest hit.

STRONG / WEAK:

```json
{
  "status": 0,
  "result": {
    "hit":   "STRONG",
    "id_hex":"019e09a95e7a...",
    "title": "...",
    "body":  "...",
    "signals": { "s_vec": 0.91, "s_lex": 0.42, "s_jaccard": 0.38, "s_ce": null }
  }
}
```

`body` is `null` on WEAK.

MISS:

```json
{
  "status": 0,
  "result": {
    "hit": "MISS",
    "fallback_retrieve": { "results": [...], "distinct_keywords": [...] },
    "signals": { "s_vec": 0.21, ... }
  }
}
```

`signals_only=true` suppresses the side effects (no `access_count` bump, no similarity sample). Use it for read-only telemetry where you don't want to taint stats.

### `GET /v1/search?text=...&top_k=10`

Hybrid retrieval via RRF over `(R_vec, R_bm25_title, R_bm25_body)`:

```json
{
  "status": 0,
  "result": {
    "results": [
      { "id_hex": "...", "title": "...", "score": 0.0314, "keywords": ["spring-boot"] }
    ],
    "distinct_keywords": [ "spring-boot", "validation", ... ]
  }
}
```

`score` is raw RRF (`Σ 1 / (60 + rank_i)`); max ≈ `0.0492` for rank-1 in all three lists.

### `GET /v1/explore?text=...&depth=3&beam=4&keywords=a,b,c`

Beam search. Optional comma-separated keyword filter applied to the seed selection.

```json
{
  "status": 0,
  "result": {
    "nodes": [{ "id_hex": "...", "title": "...", "score": 0.66, "cosine": 0.66, "depth_reached": 0 }],
    "edges": [{ "src_hex": "...", "dst_hex": "...", "kind": "semantic", "weight": 0.84 }]
  }
}
```

- `score` — log-additive beam composite (unbounded; can be negative for deep walks).
- `cosine` — bounded raw cosine of node-to-query, the actual "semantic similarity".
- `depth_reached` — step at which the node was first visited (`0` = seed).
- `edges` — only the edges **traversed** by the walk, not the surrounding subgraph.

### `GET /v1/classify?text=...`

```json
{ "status": 0, "result": { "suggested_keywords": ["spring-boot", "gotcha"] } }
```

### `POST /v1/insert`

Body (`application/json`):

```json
{
  "title":      "Required, the retrieval anchor",
  "body":       "Required, Markdown allowed",
  "keywords":   ["k1", "k2"],
  "author":     "user@host",
  "expires_at": 1735689600000,
  "supersedes": "019e09a95e7a..."
}
```

Response:

```json
{ "status": 0, "result": { "id_hex": "019e0a44...", "duplicate": false, "n_kw_edges": 3, "n_sem_edges": 2 } }
```

When `supersedes` is provided and resolves to an existing node:

1. The new node is inserted with the new content.
2. The old node's state becomes `SUPERSEDED`.
3. A `SUPERSEDES` edge connects new → old.

All three steps run in one SQLite transaction.

### `GET /v1/nodes/{id_hex}`

```json
{
  "status": 0,
  "result": {
    "id_hex":     "019e09a9...",
    "title":      "...",
    "body":       "...",
    "author":     "user@host",     // null on legacy rows
    "keywords":   ["k1", "k2"],
    "created_at": 1735000000000,   // unix ms, UTC
    "expires_at": 0,               // 0 = no expiration
    "access_count": 17
  }
}
```

Returns 404 if absent. Superseded nodes are still returned — this endpoint doesn't filter by state.

### `DELETE /v1/nodes/{id_hex}`

Hard delete. Cascades to `node_keywords`, the FTS5 mirror, `node_vec`, and every edge referencing the node. `204 No Content` on success, `404` if absent.

**Off by default**. Enable via `endpoint_delete: true` if you trust your callers and your network surface.

### `GET /v1/view`

Full graph dump for the 3D viewer:

```json
{
  "status": 0,
  "result": {
    "graph_version": 30000000606,
    "nodes": [
      { "id_hex": "...", "title": "...", "state": "active",
        "body_len": 944, "primary_keyword": "spring-boot",
        "x": 0.42, "y": -0.13, "z": 0.78 }
    ],
    "edges": [
      { "src": "...", "dst": "...", "kind": "semantic", "weight": 0.84 },
      { "src": "...", "dst": "...", "kind": "keyword", "weight": 1.0, "keyword": "spring-boot" }
    ]
  }
}
```

- `state` ∈ `"active" | "superseded" | "stale"`.
- `kind` ∈ `"semantic" | "keyword" | "supersedes" | "contradicts"`.
- `body_len` — character count of `body`, used by the viewer to size spheres.
- `primary_keyword` — alphabetically-first keyword on the node, used by the viewer for hue hashing.
- `x`, `y`, `z` — deterministic Rademacher random projection of the 1024-dim embedding into 3D. Stable across reloads; recomputed each call (cheap).
- `graph_version` — `n_nodes × 1e9 + n_edges`. The viewer polls every 3 s and skips re-render when this number is unchanged.

---

## Production deployment

Run `integrations/mcp-server/oauth_gateway.py` in front of the daemon. Keep `graftd` bound to `127.0.0.1`. The gateway:

- Mounts MCP streamable HTTP at `/mcp`.
- Proxies authenticated `/v1/*` calls to `GRAFT_UPSTREAM_HTTP` (default `http://127.0.0.1:9977`).
- Validates externally issued OIDC access tokens — issuer, audience, expiration, scopes.

Scope policy:

| Scope            | REST endpoints |
| ---------------- | -------------- |
| `graft:read`     | `GET /v1/match`, `/v1/search`, `/v1/explore`, `/v1/classify`, `/v1/nodes/{id}`, `/v1/view` |
| `graft:write`    | `POST /v1/insert` |
| `graft:admin`    | `DELETE /v1/nodes/{id}` |

Run something like:

```bash
cd integrations/mcp-server
export GRAFT_OAUTH_ISSUER_URL="https://issuer.example.com"
export GRAFT_OAUTH_RESOURCE_SERVER_URL="https://graft.example.com/mcp"
export GRAFT_OAUTH_AUDIENCE="https://graft.example.com"
uvicorn oauth_gateway:app --host 127.0.0.1 --port 8080
```

Then point your reverse proxy at `127.0.0.1:8080` and terminate TLS in front.

---

## Examples

```bash
# liveness
curl -s http://127.0.0.1:9977/v1/healthz

# cache lookup
curl -s "http://127.0.0.1:9977/v1/match?text=spring%20boot%20validation"

# hybrid retrieve
curl -s "http://127.0.0.1:9977/v1/search?text=jwt%20refresh&top_k=10"

# explore filtered by keywords
curl -s "http://127.0.0.1:9977/v1/explore?text=auth&depth=3&beam=4&keywords=spring-boot,security"

# insert with supersession
curl -s -X POST http://127.0.0.1:9977/v1/insert \
  -H 'Content-Type: application/json' \
  -d '{
    "title": "Spring Boot @Valid cascade — refined",
    "body":  "## Why\nWithout @Valid on the nested field ...",
    "keywords": ["spring-boot", "validation", "gotcha"],
    "supersedes": "019e09a95e7a7b85a96e617bff2c3e56"
  }'

# production: authenticated via the gateway
curl -s -H "Authorization: Bearer $ACCESS_TOKEN" https://graft.example.com/v1/view
```

---

## What's missing and how to improve it

- **HTTPS support in the daemon.** Today, public exposure requires a sidecar (the OAuth gateway). For trusted networks where the gateway is overkill, native TLS via OpenSSL / wolfSSL would simplify deployment.
- **Keep-alive.** The current `Connection: close` per response is fine for a viewer + sporadic CLI, terrible for a high-RPS microservice. The MCP gateway papers over this with upstream pooling, but a real `keep-alive` and HTTP/1.1 chunked encoding on the daemon would remove that layer for simple deployments.
- **Streaming responses for `/v1/view`** on very large graphs. The endpoint serialises the whole graph as one JSON object today. Above a few thousand nodes a chunked NDJSON stream would let the viewer start rendering before the entire payload is in.
- **OpenAPI spec.** No machine-readable schema is published. A small `openapi.yaml` would unlock generated clients for free.
- **Rate limiting.** None at the daemon level. The OAuth gateway has the right place for this; it currently doesn't enforce anything beyond JWT validation.
- **Metrics endpoint.** `/v1/metrics` Prometheus-style (requests by op, P99 latencies, hit rates) would unlock dashboarding without scraping the usage log.
