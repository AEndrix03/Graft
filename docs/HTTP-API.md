# HTTP REST API

The daemon ships an optional HTTP layer alongside the unix-socket protocol.
Off by default; enable it in `config.yaml`:

```yaml
http:
  enabled: true
  bind: "127.0.0.1"
  port: 9977
  # ... per-endpoint enables
```

When enabled, the daemon binds a TCP listener on `bind:port` and routes:

| Method   | Path                  | Default | Description                                    |
| -------- | --------------------- | ------- | ---------------------------------------------- |
| `GET`    | `/v1/healthz`         | always  | Liveness probe, no auth required.              |
| `GET`    | `/v1/match`           | on      | Cache lookup with multi-signal gating.         |
| `GET`    | `/v1/search`          | on      | Hybrid top-k retrieval (RRF over vec+BM25).    |
| `GET`    | `/v1/explore`         | on      | Beam-search graph walk from a semantic seed.   |
| `GET`    | `/v1/classify`        | on      | Suggest keywords for a draft title.            |
| `POST`   | `/v1/insert`          | on      | Save a new node (supersession optional).       |
| `GET`    | `/v1/nodes/{id_hex}`  | on      | Fetch a single node (title, body, metadata).   |
| `DELETE` | `/v1/nodes/{id_hex}`  | **off** | Remove a node — sensitive, opt-in.             |
| `GET`    | `/v1/view`            | on      | Full graph dump (nodes + edges) for the viewer.|
| `GET`    | `/`                   | always  | Static SPA bundle from `http.viewer_path`.     |

Each `/v1/*` endpoint can be disabled independently (`endpoint_delete: false` etc.).

## Auth

The C daemon is local-first and does not validate OAuth/JWT tokens. Keep it
bound to `127.0.0.1` and put `integrations/mcp-server/oauth_gateway.py` in
front of `/v1/*` for production.

Gateway scope policy:

| Scope | REST endpoints |
| ----- | -------------- |
| `memgraph:read` | `GET /v1/match`, `/v1/search`, `/v1/explore`, `/v1/classify`, `/v1/nodes/{id}`, `/v1/view` |
| `memgraph:write` | `POST /v1/insert` |
| `memgraph:admin` | `DELETE /v1/nodes/{id}` |

`/v1/healthz` is unauthenticated for liveness checks.

## Bind & security defaults

- `bind: "127.0.0.1"` — local-only. The daemon **does not** bind on `0.0.0.0`
  unless you flip it explicitly. Local-first is the design.
- `endpoint_delete: false` is the default because deleting nodes via HTTP is
  high-blast-radius (no confirmation step).
- There is no HTTPS layer in the daemon. Public deployments should terminate
  TLS in front of the OAuth gateway and keep the daemon locked to `127.0.0.1`.

## Response envelope

All `/v1/*` endpoints return the same JSON shape:

```json
{ "status": 0, "result": { ... }, "error": null }
```

- `status: 0` → success. Non-zero is an internal error code (`mg_err_t`).
- `result` carries the per-endpoint payload.
- `error` is a human-readable string when `status != 0`.

HTTP status codes:

| Code | When                                                                   |
| ---- | ---------------------------------------------------------------------- |
| 200  | success                                                                |
| 201  | `POST /v1/insert` created a new node                                   |
| 204  | `DELETE /v1/nodes/{id}` succeeded (no body)                            |
| 400  | malformed query string or JSON body                                    |
| 401  | returned by the OAuth gateway for missing/malformed/expired tokens     |
| 404  | unknown route, disabled endpoint, or `id_hex` not found                |
| 500  | daemon error (look at stderr / `~/.lmemorygraph/memgraphd.err.log`)    |

## Endpoints

### `GET /v1/match?text=...`

Cache lookup. Embeds the input, runs vector top-1, applies the verify
pipeline (trigram Jaccard + cosine + optional cross-encoder).

**Response — STRONG / WEAK hit:**

```json
{
  "status": 0,
  "result": {
    "hit": "STRONG",
    "id_hex": "019e09a95e7a...",
    "title": "Spring Boot @Valid cascade on nested DTOs ...",
    "body":  "Without @Valid on the nested field, ...",
    "signals": {
      "s_vec": 0.91, "s_lex": 0.42, "s_jaccard": 0.38, "s_ce": null
    }
  }
}
```

`body` is `null` on `WEAK` hits.

**Response — MISS:**

```json
{
  "status": 0,
  "result": {
    "hit": "MISS",
    "fallback_retrieve": {
      "results": [ { "id_hex": "...", "title": "...", "score": 0.014 } ]
    },
    "signals": { "s_vec": 0.21, ... }
  }
}
```

The fallback list is capped at `retrieval.query_fallback_top_k` (default 5).

### `GET /v1/search?text=...&top_k=10`

Hybrid retrieval. Returns nodes ranked by RRF score over three lists
(vector cosine, BM25 over title, BM25 over body).

```json
{
  "status": 0,
  "result": {
    "results": [
      { "id_hex": "...", "title": "...", "score": 0.0314,
        "keywords": ["spring-boot", "validation"] }
    ],
    "distinct_keywords": [ "spring-boot", "validation", ... ]
  }
}
```

`score` is the raw RRF (`Σ 1/(k+rank_i)` with `k=60`); the theoretical max
is `3/61 ≈ 0.0492` for a doc that's rank 1 in all three lists.

### `GET /v1/explore?text=...&depth=3&beam=4&keywords=a,b,c`

Beam-search walk. Optional `keywords` is a comma-separated filter applied
to the seed selection.

```json
{
  "status": 0,
  "result": {
    "nodes": [
      { "id_hex": "...", "title": "...", "score": 0.66, "cosine": 0.66, "depth_reached": 0 }
    ],
    "edges": [
      { "src_hex": "...", "dst_hex": "...", "kind": "semantic", "weight": 0.84 }
    ]
  }
}
```

- `score` is the log-additive beam composite (used for ranking, unbounded —
  can be negative for deep walks because it accumulates `log(weight) +
  α·log(cosine)`).
- `cosine` is the raw cosine of node-to-query — bounded `[-1, 1]`, the
  meaningful "semantic similarity" measure.
- `depth_reached` is the step at which the node was first visited (0 = seed).
- `edges` lists only the edges actually **traversed** by the search. Seeds
  have no incoming edge in this list.

### `GET /v1/classify?text=...`

Suggest 3–6 keywords for a draft title.

```json
{ "status": 0, "result": { "suggested_keywords": ["spring-boot", "gotcha"] } }
```

### `POST /v1/insert`

Body — `application/json`:

```json
{
  "title":     "Required, the retrieval anchor",
  "body":      "Required, Markdown allowed",
  "keywords":  ["k1", "k2"],
  "author":    "user@host",
  "expires_at": 1735689600000,
  "supersedes": "019e09a95e7a..."
}
```

`author`, `expires_at`, `supersedes` are optional. `keywords` defaults to `[]`.

**Supersession**: when `supersedes` is provided and resolves to an existing
node, the insert is atomic:
1. The new node is inserted with the new content.
2. The old node's state becomes `SUPERSEDED`.
3. A `SUPERSEDES` edge connects the new node to the old.

All three steps run in a single SQLite transaction. After supersession,
`/v1/match`, `/v1/search`, `/v1/explore` filter superseded nodes out of
candidate selection (they're still reachable via `/v1/nodes/{id}`).

**Response:**

```json
{
  "status": 0,
  "result": {
    "id_hex": "019e0a4466...",
    "duplicate": false,
    "n_kw_edges":  3,
    "n_sem_edges": 2
  }
}
```

`duplicate: true` when the content hash matched an existing node — the
returned `id_hex` is the existing one, no new node was created.

### `GET /v1/nodes/{id_hex}`

Fetch a single node by its 32-char hex id.

```json
{
  "status": 0,
  "result": {
    "id_hex":      "019e09a95e7a...",
    "title":       "...",
    "body":        "...",
    "author":      "user@host",   // null for legacy rows
    "keywords":    ["k1", "k2"],
    "created_at":  1735000000000, // unix ms, UTC
    "expires_at":  0,             // 0 = no expiration
    "access_count": 17
  }
}
```

Returns 404 if the id doesn't exist. Superseded nodes are still returned —
this endpoint doesn't filter by state.

### `DELETE /v1/nodes/{id_hex}`

Hard-delete the node. Cascades to:
- `node_keywords`, `node_fts`, `node_vec` (the embedding)
- `edges` where it appears as src or dst

Returns `204 No Content` on success, `404` if absent. **Off by default** —
flip `endpoint_delete: true` in `config.yaml` to enable.

### `GET /v1/view`

Full graph dump for the 3D viewer.

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
      { "src": "...", "dst": "...", "kind": "keyword",
        "weight": 1.0, "keyword": "spring-boot" }
    ]
  }
}
```

- `state ∈ {"active", "superseded", "stale"}`.
- `kind ∈ {"semantic", "keyword", "supersedes", "contradicts"}`.
- `body_len` is the character count of `body` — used by the viewer to size
  spheres proportionally to content depth.
- `primary_keyword` is the alphabetically-first keyword on the node, used by
  the viewer for color hashing (so nodes sharing a keyword cluster visually).
- `x`, `y`, `z` are server-side projected 3D coordinates derived from the
  1024-dim embedding via deterministic Rademacher random projection (fixed
  seed). Stable across reloads; recomputed each `/v1/view` call but cheap.
- `graph_version` is `n_nodes × 1e9 + n_edges` — a coarse change hint for
  client-side polling. The viewer refetches every 3s and skips re-render
  when the version is unchanged.

### `GET /v1/healthz`

```json
{ "status": "ok", "service": "memgraphd" }
```

Always served, never auth-gated. Use for container probes.

## Examples

```bash
# health check
curl -s http://127.0.0.1:9977/v1/healthz

# match
curl -s "http://127.0.0.1:9977/v1/match?text=spring%20boot%20validation"

# insert with supersession
curl -s -X POST http://127.0.0.1:9977/v1/insert \
  -H 'Content-Type: application/json' \
  -d '{
    "title": "Spring Boot @Valid cascade — refined",
    "body":  "## Why\nWithout @Valid on the nested field ...",
    "keywords": ["spring-boot", "validation", "gotcha"],
    "supersedes": "019e09a95e7a7b85a96e617bff2c3e56"
  }'

# explore filtered by keywords
curl -s "http://127.0.0.1:9977/v1/explore?text=auth&depth=3&beam=4&keywords=spring-boot,security"

# production gateway with an externally issued OIDC access token
curl -s -H "Authorization: Bearer $ACCESS_TOKEN" https://memgraph.example.com/v1/view
```

## Wire-format notes

- Request bodies are JSON. The daemon parses a small, controlled subset (the
  insert handler accepts the documented shape; unknown keys are ignored).
- Response bodies are JSON encoded from the same mpack the unix-socket layer
  uses internally — so the `result` shape is the source of truth across both
  transports.
- `Connection: close` is sent on every response. No keep-alive — the
  controlling assumption is that this is a local-first inspection surface,
  not a high-throughput service. For heavy programmatic access, use the unix
  socket (mpack-framed) directly.
