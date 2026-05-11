# Architecture

Graft is split into two pieces. Understanding the split is the whole story.

```
┌────────────────────────────┐
│  Any agent or tool         │
│  (Claude Code, Codex, ...) │
└──────────────┬─────────────┘
               │ exec()
               ▼
┌────────────────────────────┐         ┌─────────────────────────────────────┐
│  graft (CLI)               │  pipe   │  graftd (daemon)                    │
│  thin client; 1 RTT/cmd    │ ──────► │  loads BGE-M3 once,                 │
│  parses argv, prints JSON  │  AF_UNIX│  owns the SQLite + sqlite-vec DB    │
└────────────────────────────┘  socket │  + FTS5, dispatches per op          │
                                       └──────────────────┬──────────────────┘
                                                          │
                                       optional TCP /v1/* ▼
                                       ┌─────────────────────────────────────┐
                                       │  Browser 3D viewer · REST clients · │
                                       │  OAuth/OIDC gateway · custom svc    │
                                       └─────────────────────────────────────┘
```

## Why a daemon

The two facts that drive the split:

1. **The embedding model is expensive to load.** BGE-M3 Q8_0 is ~600 MB on disk and roughly the same in RAM. Loading it on every CLI invocation would put a second-level cost into every "did I solve this before?" question and would be unusable.
2. **SQLite WAL plays nicely with one writer.** The daemon is the one writer. Concurrent CLI calls multiplex over the socket and serialise inside the daemon's dispatcher.

So the CLI is intentionally **thin**:

- ~600 LOC
- parses argv, encodes a MessagePack request, connects to a unix socket, reads one response frame, prints it.

And the daemon is intentionally **fat**:

- owns the embedding context (`mg_embed_ctx_t`)
- owns the storage handle (`mg_storage_t`)
- owns the verifier (`mg_verify_ctx_t`)
- owns the config
- accepts client connections in a thread-per-connection loop
- dispatches each request through `mg_dispatch()` to the right `mg_op_*` handler

If the daemon dies, the CLI's first call after that auto-restarts it (`src/cli/autostart.c`). The user does not manage process lifecycle.

## The request lifecycle, from `graft insert` to a row on disk

This is what happens when you run `graft insert --title "..." --body "..." --keyword foo`:

```
1.  argv parsed                              (src/cli/main.c::build_insert)
2.  request frame encoded as MessagePack:    {op: "insert", args: {title, body, keywords, author?, expires_at?, supersedes?}}
3.  CLI applies profile env vars             (GRAFT_SOCKET / GRAFT_DB_PATH)
4.  CLI connects to AF_UNIX socket           (mg_daemon_socket_connect)
        ↳ if connect fails, mg_autostart_daemon() spawns graftd and polls until ready
5.  CLI writes 4-byte LE length + payload    (mg_wire_write_frame)
6.  daemon reads frame, parses op            (mg_dispatch)
7.  daemon routes "insert" to mg_op_insert
        7a. embed(title) via llama.cpp       → 1024-dim f32 vector, L2-normalised
        7b. upsert each keyword              → keyword_id
        7c. compute content_hash (BLAKE3 over title|body|sorted_keywords)
        7d. lookup by content_hash           → if found, return duplicate=true
        7e. build edges:
              - for each keyword: vector_topk_by_keyword(q, kw_id, K=5) → KEYWORD edges
              - then vector_topk(q, K=20) + MMR diversification        → SEMANTIC edges
        7f. INSERT in a single SQLite transaction:
              - nodes row
              - node_keywords link rows
              - sqlite-vec embedding row
              - edges rows
              - if `supersedes`: state ← SUPERSEDED on old + a SUPERSEDES edge
8.  daemon encodes mpack response            {status: 0, result: {id_hex, n_kw_edges, n_sem_edges, duplicate}}
9.  daemon writes frame back                 (mg_wire_write_frame)
10. CLI reads frame, pretty-prints JSON      (print_value)
11. CLI appends to ~/.graft/usage.jsonl      (mg_usage_log_append)
12. CLI exits; daemon keeps the model loaded for the next call
```

A `graft query` follows the same path but a different op-handler. The shape is identical: encode request, dispatch, write response.

## Wire format

Every frame is `uint32_t length (little-endian) | MessagePack payload`.

The payload is a map with two keys, `op` (string) and `args` (map). The response is a map with `status` (int, `MG_OK = 0`), `result` (any), and optionally `error` (string when `status != 0`).

The wire enum:

| Op             | Handler              | Description |
| -------------- | -------------------- | ----------- |
| `classify`     | `mg_op_classify`     | Suggest keywords for a draft title. |
| `insert`       | `mg_op_insert`       | Save a node + edges in one transaction. |
| `query`        | `mg_op_query`        | Verified cache lookup. |
| `retrieve`     | `mg_op_retrieve`     | Hybrid top-k via RRF. |
| `explore`      | `mg_op_explore`      | Beam-search walk with MMR. |
| `get`          | `mg_op_get`          | Fetch a single node by id. |
| `delete`       | `mg_op_delete`       | Hard-delete by id (cascades). |
| `stats`        | `mg_op_stats`        | Similarity-distribution percentiles. |
| `consolidate`  | `mg_op_consolidate`  | Maintenance pass (prune, dedup, report). |
| `view`         | `mg_op_view`         | Full graph dump for the viewer. |

The full list lives in `include/graft/wire.h`.

## Modules

| Module          | Header                  | Responsibility |
| --------------- | ----------------------- | -------------- |
| storage         | `include/graft/storage.h`   | SQLite + sqlite-vec + FTS5. Schema, migrations, all CRUD. |
| embed           | `include/graft/embed.h`     | llama.cpp wrapper. Loads BGE-M3, computes L2-normalised vectors. |
| verify          | `include/graft/verify.h`    | Multi-signal hit gating (trigram Jaccard + cosine + optional CE). |
| ops             | `include/graft/ops.h`       | Op handlers: classify / insert / query / retrieve / explore / get / stats / consolidate / delete / view. |
| http            | `include/graft/http.h`      | Optional REST + viewer transport. |
| wire            | `include/graft/wire.h`      | MessagePack framing helpers. |
| types           | `include/graft/types.h`     | `mg_node_t`, `mg_edge_t`, `mg_embedding_t`, ids, hashes. |
| config          | `include/graft/config.h`    | YAML loader + defaults. |

Every header is intentionally small. The internal C APIs are not promised to be stable — the **CLI JSON schema** is the public contract.

## Concurrency model

- One **listener thread** in the daemon accepts socket connections.
- Each accepted connection runs `handle_client()` on its own pthread (detached).
- Inside the handler, operations are dispatched against shared `mg_ctx_t` state (storage, embed, verify).
- The storage layer holds an internal mutex on the SQLite handle for write paths. Read paths use SQLite's own concurrency (WAL allows readers and a single writer concurrently).
- The embed context is thread-safe at the llama.cpp level for the inference call used by graft (CPU pool sized by `embedding.threads`).
- The optional HTTP layer runs on its own background thread and **reuses the same dispatcher** — the HTTP handlers build mpack requests, run them through `mg_dispatch`, and convert the response to JSON.

## Why no library bindings

Graft is shipped as binaries, not as a `pip`/`cargo`/`npm` SDK. The contract is "spawn a subprocess and read JSON". This is by design:

- One artifact, not N.
- No language-runtime drift between the client and the daemon — every agent that can run a child process can use it.
- Easier to instrument (`strace`, `ltrace`, structured shell logs).
- Easier to swap implementations later without breaking callers.

If you want a thin SDK in a specific language, wrap the CLI calls. The shape is so small that the wrapper is usually 30–50 lines.

---

## What's missing and how to improve it

- **A reusable C library API.** `graft_core` is built as a static library but its public headers (`include/graft/*.h`) are not yet stable. Decide which symbols to commit to, version the headers, ship a `pkg-config` file. That would let downstream C / Rust / Go programs link directly.
- **Connection pooling on the CLI side.** Each CLI invocation opens and closes its own socket. For tight loops (e.g. ingesting a large folder via `/learn`) a persistent client process would shave ~5–10 ms of socket-setup cost per call.
- **A proper backpressure model on the daemon.** Right now each accepted connection becomes a pthread with no admission control. With aggressive concurrent clients (e.g. a large microservice fleet), a small worker pool with a bounded queue would be safer.
- **Structured logs.** The daemon writes free-form `fprintf(stderr, ...)`. Switching to a small line-oriented JSON log would make external observability (Vector, Loki, ...) drop-in.
- **Tracing.** No spans, no propagated trace context. Adding OpenTelemetry on the HTTP path is the cheapest high-impact win — it lets ops people see the L1 / L2 / L3 cost picture end-to-end.
