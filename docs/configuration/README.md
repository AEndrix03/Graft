# Configuration

Two surfaces:

- **`config.yaml`** — the daemon's persistent settings. Lives at `~/.graft/config.yaml` for user installs (auto-detected). The example is `config.example.yaml` at the repo root, which the Homebrew formula and the install script drop in the right place.
- **Environment variables** — per-invocation overrides on top of the YAML. Mostly used to switch profile / db / socket without rewriting the file.

The loader is `src/config/config.c`. Defaults live in `mg_config_defaults`. Missing keys fall back to defaults, so a minimal config file is short.

---

## `config.yaml`

### Full example

```yaml
daemon:
  socket_path: "/tmp/graft.sock"
  db_path:     "./graft.db"

embedding:
  model_path:     "./models/bge-m3.gguf"
  threads:        4
  ctx_size:       8192
  hardware_accel: false   # GPU offload (CUDA / ROCm). Requires matching llama.cpp build.

verification:
  cross_encoder_enabled:    false
  cross_encoder_model_path: "./models/bge-reranker-v2-m3.gguf"
  nli_enabled:              false

cache:
  strong_hit_min_ce:  0.6
  strong_hit_min_lex: 0.15
  weak_hit_min_vec:   0.85
  min_lex_overlap:    0.05

retrieval:
  top_k:                25
  rrf_k_const:          60
  query_fallback_top_k: 5

edges:
  edge_keyword_min:    0.5
  edge_semantic_min:   0.6
  edge_keyword_topk:   5
  edge_semantic_topk:  20
  mmr_lambda:          0.7

explore:
  default_beam:   4
  default_depth:  3
  decay_gamma:    0.85
  alpha:          0.5

http:
  enabled:           false
  bind:              "127.0.0.1"
  port:              9977
  endpoint_match:    true
  endpoint_search:   true
  endpoint_explore:  true
  endpoint_classify: true
  endpoint_insert:   true
  endpoint_delete:   false      # sensitive; opt-in
  endpoint_view:     true
  viewer_path:       "viewer/dist"
```

### Key reference

#### `daemon`

| Key | Default | What it does |
| --- | ------- | ------------ |
| `socket_path` | `/tmp/graft.sock` | The AF_UNIX socket the daemon listens on. Override per-profile via `GRAFT_SOCKET`. |
| `db_path`     | `./graft.db`      | Path to the SQLite DB. Override per-profile via `GRAFT_DB_PATH`. |

> The CLI sets `GRAFT_SOCKET` and `GRAFT_DB_PATH` automatically based on the active profile, so you rarely touch these directly in `config.yaml`.

#### `embedding`

| Key | Default | What it does |
| --- | ------- | ------------ |
| `model_path`     | `./models/bge-m3.gguf` | Where the GGUF model lives. Homebrew rewrites this to the Cellar path on install. |
| `threads`        | `4` | llama.cpp CPU pool size for the embedding call. |
| `ctx_size`       | `8192` | Context window. BGE-M3 supports up to ~8 K tokens. |
| `hardware_accel` | `false` | Offload all layers to GPU. **Requires** llama.cpp built with `-DGGML_CUDA=ON` or `-DGGML_HIP=ON`. Daemon refuses to start if mismatched. |

#### `verification`

| Key | Default | What it does |
| --- | ------- | ------------ |
| `cross_encoder_enabled` | `false` | Turn on the cross-encoder reranker (currently a stub — see roadmap). |
| `cross_encoder_model_path` | `./models/bge-reranker-v2-m3.gguf` | Path to the reranker GGUF. Only consulted when `cross_encoder_enabled: true`. |
| `nli_enabled` | `false` | Reserved for the future contradiction-detection layer. No-op today. |

#### `cache` (the `query` gating thresholds)

| Key | Default | What it gates |
| --- | ------- | ------------- |
| `strong_hit_min_ce`  | `0.6` | Minimum cross-encoder score for STRONG, **only when CE is enabled**. |
| `strong_hit_min_lex` | `0.15` | Minimum trigram Jaccard for STRONG. Hard floor on lexical overlap. |
| `weak_hit_min_vec`   | `0.85` | Minimum cosine for WEAK. Note: STRONG has a hard-coded `s_vec ≥ 0.7` floor in code. |
| `min_lex_overlap`    | `0.05` | Minimum trigram Jaccard for WEAK. |

Read [`retrieval/`](../retrieval/) for the full gating formula.

#### `retrieval`

| Key | Default | What it does |
| --- | ------- | ------------ |
| `top_k`                | `25` | Default `top_k` for `graft retrieve`. Internally capped at 256. |
| `rrf_k_const`          | `60` | The `k` in `1 / (k + rank_i)`. Standard RRF value. |
| `query_fallback_top_k` | `5`  | Cap on the `fallback_retrieve` list inside a `query` MISS response. Keeps the agent's context light on bad queries. |

#### `edges`

| Key | Default | What it does |
| --- | ------- | ------------ |
| `edge_keyword_min`   | `0.5`  | Cosine floor for a `MG_EDGE_KEYWORD` edge to be created. |
| `edge_semantic_min`  | `0.6`  | Cosine floor for a `MG_EDGE_SEMANTIC` edge to be created. |
| `edge_keyword_topk`  | `5`    | Top-k per keyword when building keyword edges. |
| `edge_semantic_topk` | `20`   | Pool size for MMR-driven semantic edge selection. |
| `mmr_lambda`         | `0.7`  | MMR balance: 1.0 = pure relevance, 0.0 = pure diversity. |

#### `explore`

| Key | Default | What it does |
| --- | ------- | ------------ |
| `default_beam`  | `4`    | Beam width when the caller doesn't supply one. |
| `default_depth` | `3`    | Beam depth when the caller doesn't supply one. |
| `decay_gamma`   | `0.85` | Per-step depth penalty. |
| `alpha`         | `0.5`  | Weight of semantic-to-query relevance vs traversed edge weight in the beam score. |

#### `http`

| Key | Default | What it does |
| --- | ------- | ------------ |
| `enabled`           | `false` | Start the optional HTTP TCP listener and viewer. |
| `bind`              | `127.0.0.1` | Local-first. Do **not** flip this to `0.0.0.0` without an OAuth gateway in front. |
| `port`              | `9977` | TCP port. |
| `endpoint_match`    | `true` | `GET /v1/match` |
| `endpoint_search`   | `true` | `GET /v1/search` |
| `endpoint_explore`  | `true` | `GET /v1/explore` |
| `endpoint_classify` | `true` | `GET /v1/classify` |
| `endpoint_insert`   | `true` | `POST /v1/insert` |
| `endpoint_delete`   | `false`| `DELETE /v1/nodes/{id}`. Off by default — sensitive. |
| `endpoint_view`     | `true` | `GET /v1/view` (used by the 3D viewer). |
| `viewer_path`       | `viewer/dist` | Directory containing the built SPA bundle. The Homebrew formula rewrites this to the Cellar location. |

`GET /v1/healthz` is always served, never gated.

---

## Where the daemon looks for `config.yaml`

`src/cli/autostart.c::mg_resolve_config` (used when the CLI auto-starts the daemon):

1. `$GRAFT_CONFIG` if set and the file exists.
2. `$GRAFT_HOME/config.yaml` if set.
3. `~/.graft/config.yaml` (POSIX) or `%USERPROFILE%\.graft\config.yaml` (Windows).
4. `./config.yaml` in the cwd.
5. `./config.example.yaml` in the cwd.
6. `<cli-binary-dir>/../config.example.yaml` (dev fallback for repo checkouts).

For `graftd` invoked directly, pass `--config <path>`. There is no implicit walk for the standalone daemon.

---

## Environment variables

Read by the **CLI** (see `src/cli/main.c`, `src/cli/profile.c`):

| Variable | Default | Effect |
| -------- | ------- | ------ |
| `GRAFT_PROFILE`   | `default` | Active profile. The CLI derives socket / DB paths from it. |
| `GRAFT_HOME`      | `~/.graft` (POSIX) or `%USERPROFILE%\.graft` | Root of profiles, sockets, usage log. |
| `GRAFT_SOCKET`    | _(per-profile path)_ | Force a specific socket path. |
| `GRAFT_DB_PATH`   | _(per-profile path)_ | Force a specific DB path. |
| `GRAFT_CONFIG`    | _(auto-discovered)_ | Force a specific `config.yaml`. |
| `GRAFT_AUTHOR`    | `<user>@<host>` | Default author on `insert`. Empty string opts out. |
| `GRAFT_USAGE_LOG` | `$GRAFT_HOME/usage.jsonl` | Force a specific usage log path. |
| `SHELL`           | OS default | Used to detect the right export syntax in `graft profile set`. |

Read by the **daemon** (`src/daemon/main.c::apply_env_overrides`):

| Variable | Effect |
| -------- | ------ |
| `GRAFT_SOCKET` | Overrides `daemon.socket_path` from `config.yaml`. |
| `GRAFT_DB_PATH` | Overrides `daemon.db_path` from `config.yaml`. |

Read by the **MCP gateway** (`integrations/mcp-server/oauth_gateway.py`):

| Variable | Effect |
| -------- | ------ |
| `GRAFT_BIN` | Path to `graft` CLI for stdio / MCP tools. |
| `GRAFT_OAUTH_ISSUER_URL` | OIDC issuer publishing discovery metadata. Required for `oauth_gateway`. |
| `GRAFT_OAUTH_RESOURCE_SERVER_URL` | Public MCP resource URL (usually `https://host/mcp`). |
| `GRAFT_OAUTH_AUDIENCE` | Expected JWT `aud`. |
| `GRAFT_OAUTH_REQUIRED_SCOPES` | Baseline scopes advertised in protected-resource metadata. Default `graft:read`. |
| `GRAFT_OAUTH_JWKS_CACHE_SECONDS` | JWKS cache lifetime. Default `300`. |
| `GRAFT_UPSTREAM_HTTP` | Where to proxy `/v1/*` calls. Default `http://127.0.0.1:9977`. |

---

## What's missing and how to improve it

- **Per-profile `config.yaml` overrides.** Today there is one global config. A `~/.graft/profiles/<name>/config.yaml` overlay would let, e.g., `work` run on GPU and `personal` run on CPU.
- **Schema validation on load.** The YAML loader is forgiving — unknown keys are silently ignored, typos in known keys fall through to defaults. A `graft config check` mode that printed warnings for unknown or out-of-range values would help.
- **Hot reload.** `config.yaml` is read once at daemon start. Changes require a restart. A `SIGHUP` handler that re-reads thresholds (the safe subset, not `db_path`) would be a small but appreciated win.
- **`graft config show`** that prints the **resolved** config (defaults + YAML + env) the daemon actually loaded. Today you have to read the log line at startup.
- **Docs cross-link from the daemon's startup banner**. The daemon already prints `listening on /tmp/graft.sock` — it could also print the resolved config path and a one-line link to this page for first-time users.
