# Storage

Graft persists everything in **one SQLite file**.

That file holds:

- the nodes (your saved memories),
- their **1024-dim BGE-M3 embeddings**, stored in [sqlite-vec](https://github.com/asg017/sqlite-vec) virtual tables for fast cosine top-k,
- a **FTS5 mirror** of `(title, body)` for BM25 lexical retrieval,
- the **keywords** and the `node ↔ keyword` link table,
- the **edges** (semantic, keyword, supersedes, contradicts) with their weights,
- a small **`similarity_samples`** table the daemon uses to compute the percentiles you see in `graft stats`.

Backups: `cp graft.db dest/`. Migration to a new machine: `graft profile export work --path work.graftprofile` and `graft profile import --name work --file work.graftprofile`. The file **is** a regular SQLite DB — you can open it in `sqlite3` and inspect it.

## Schema (current — v3)

```sql
CREATE TABLE nodes (
  id            BLOB PRIMARY KEY,             -- UUIDv7, 16 bytes
  content_hash  BLOB NOT NULL UNIQUE,         -- BLAKE3 of title + body + sorted keywords
  title         TEXT NOT NULL,
  body          TEXT NOT NULL,
  author        TEXT,                         -- "user@host", NULL on legacy rows
  created_at    INTEGER NOT NULL,             -- unix ms
  expires_at    INTEGER NOT NULL DEFAULT 0,   -- unix ms; 0 = no expiration
  last_access   INTEGER NOT NULL,
  access_count  INTEGER NOT NULL DEFAULT 0,
  state         INTEGER NOT NULL DEFAULT 0,   -- 0 active, 1 stale, 2 superseded
  origin        INTEGER NOT NULL DEFAULT 0    -- 0 local, 1 remote (profile sync)
);

CREATE VIRTUAL TABLE node_fts USING fts5(
  title, body,
  content='nodes', content_rowid='rowid',
  tokenize='unicode61 remove_diacritics 2'
);

CREATE TABLE keywords (
  id            INTEGER PRIMARY KEY AUTOINCREMENT,
  text          TEXT NOT NULL UNIQUE COLLATE NOCASE,
  canonical_id  INTEGER REFERENCES keywords(id),   -- optional canonicalisation
  embedding     BLOB                                -- optional precomputed embedding
);

CREATE TABLE node_keywords (
  node_id     BLOB NOT NULL REFERENCES nodes(id) ON DELETE CASCADE,
  keyword_id  INTEGER NOT NULL REFERENCES keywords(id),
  PRIMARY KEY (node_id, keyword_id)
);

CREATE TABLE edges (
  src         BLOB NOT NULL REFERENCES nodes(id) ON DELETE CASCADE,
  dst         BLOB NOT NULL REFERENCES nodes(id) ON DELETE CASCADE,
  kind        INTEGER NOT NULL,            -- 0 keyword, 1 semantic, 2 contradicts, 3 supersedes
  keyword_id  INTEGER,                     -- only set for kind=keyword
  weight      REAL NOT NULL
);
CREATE UNIQUE INDEX idx_edges_unique
  ON edges (src, dst, kind, COALESCE(keyword_id, -1));

CREATE TABLE similarity_samples (
  ts      INTEGER NOT NULL,
  kind    INTEGER NOT NULL,    -- 0 insert_topk, 1 query_top1
  cosine  REAL NOT NULL
);
```

Plus two virtual tables managed by `sqlite-vec` for the embedding side (`node_vec` storing one 1024-dim row per node, and an internal staging table during top-k).

### PRAGMAs the daemon sets on open

```sql
PRAGMA journal_mode = WAL;       -- readers don't block writers
PRAGMA synchronous  = NORMAL;    -- safe and fast on WAL
PRAGMA foreign_keys = ON;        -- cascades from node deletes
PRAGMA mmap_size    = 268435456; -- 256 MiB mmap window
```

### Migrations

Schema version is detected at open time, in `src/storage/schema.c`:

- **v2** rename pass — columns `summary` / `detail` were renamed to `title` / `body`, FTS5 / triggers were rebuilt with the new column names, the `author` and `expires_at` columns were added. Idempotent — a fresh DB is a no-op.
- **v3** added `origin` (local vs. remote-mirrored) and an index on it. Powers profile sync.

If you see a schema-related error on first open after a pull, it usually means the daemon was running mid-update: stop it (`pkill graftd` or close the CLI shell), and re-run `graft stats`.

## Idempotency: the content hash

Every insert computes `BLAKE3(title || '\0' || body || '\0' || sorted_keywords)` and looks the result up in `nodes.content_hash`.

- Found → return the existing `id_hex` with `duplicate: true`. **No** new row, **no** new edges, **no** wasted embedding pass.
- Not found → run the full insert pipeline (embed, edges, transaction).

The hash deliberately ignores `author`, `created_at`, and `expires_at`. The same memory saved twice by different users on different days deduplicates correctly. If you want to "tag" who saved a memory without breaking dedup, use a separate keyword (`#by-andrea`), not the author field.

## Supersession (atomic edits)

When you call `insert` with a `supersedes` field, the following **all happens inside one transaction**:

1. The new node is inserted with its content and edges.
2. The old node's `state` becomes `MG_NODE_SUPERSEDED` (2).
3. A `MG_EDGE_SUPERSEDES` edge connects new → old.

After that:

- `query`, `retrieve`, and `explore` filter `state != ACTIVE` out of candidate selection.
- `get <old_id>` still returns the old node — history is preserved.
- The viewer renders superseded nodes in muted gray.
- A red `SUPERSEDES` edge in the viewer makes the chain visible.

This is what powers click-to-edit in the browser viewer. Saving an edit there is a **single REST round-trip**: `POST /v1/insert` with `supersedes: <old_id>`.

## Vector search (`sqlite-vec`)

`sqlite-vec` is loaded as a SQLite extension. Top-k cosine is one SQL statement:

```sql
SELECT id, distance
  FROM node_vec
 WHERE embedding MATCH :query
 ORDER BY distance
 LIMIT :k;
```

Internally `distance` is L2 distance over L2-normalised vectors, which is monotonically equivalent to cosine. Graft normalises every embedding at write time (`mg_embed_text`) and on the query side too, so the order is correct.

Top-k constrained by keyword is the same query with a `JOIN node_keywords` predicate. Used by the insert path to build keyword edges.

## FTS5 (lexical / BM25)

`node_fts` is an FTS5 virtual table mirroring `(title, body)`. Triggers keep it in sync with `nodes` on insert / update / delete. Tokenizer is `unicode61 remove_diacritics 2` — diacritic-folded, Unicode case-folded, default tokenisation.

The daemon issues **two** FTS5 queries per `retrieve` call: one restricted to `title`, one restricted to `body`. They become two of the three lists fed into RRF. Splitting them out is intentional — a query that lexically matches the title is a very different signal from one that only matches the body.

## Statistics

The daemon writes one row to `similarity_samples` for each candidate at insert time (`kind=0`) and for the top-1 at query time (`kind=1`). `graft stats` computes `p25 / p50 / p75 / p90 / p95 / p99` per kind on the fly. This is how you set good thresholds for your corpus.

## Consolidate

`graft consolidate` runs:

1. `mg_storage_prune_expired` — delete nodes whose `expires_at` is past.
2. Remove legacy / invalid graph rows: orphan edges, orphan `node_keywords`, duplicate edges that violate the unique index, edges with weights outside `[0, 1]`.
3. `ANALYZE` to refresh planner statistics so subsequent queries pick good indexes.
4. Compute and emit a report: `n_nodes`, `n_edges`, `n_keywords`, `isolated_nodes`, `physical_bidirectional_pairs`, `contradictions_found`.

The pass is **non-destructive** for semantic content — it never silently merges two nodes by similarity. Manual content consolidation is a future feature; today you do it explicitly through the viewer or through `/memoryze` from your agent.

## What the daemon does at startup

`src/storage/storage.c::mg_storage_open` does:

1. `sqlite3_open(db_path)` with the appropriate flags (`OPEN_READWRITE | OPEN_CREATE`).
2. `sqlite3_enable_load_extension`, then load `sqlite-vec` (it's statically linked, registered via its init function).
3. Run the schema migration block: detect the schema version, apply v2 / v3 if needed.
4. Run `mg_storage_apply_schema` to create any missing tables / indexes / triggers (idempotent).
5. Prepare the cached statements used on the hot path (top-k, FTS5, edge inserts).

If any of these steps fails the daemon does **not** start. The CLI prints the error and exits non-zero.

## Threading

The storage handle is owned by the daemon. All write paths run under a mutex. Reads use SQLite's WAL concurrency — multiple readers + one writer is the design.

The CLI is single-threaded by construction (one request per invocation). Concurrent CLI processes contend at the socket layer, then at the daemon's pthread pool, then at the storage mutex. Throughput on warm I/O is ~hundreds of inserts per second on a laptop, dominated by the embedding pass.

---

## What's missing and how to improve it

- **Real cross-process WAL checkpointing knobs.** The defaults are fine for tens of thousands of nodes; they have not been tuned at the multi-million-node scale (no such corpus exists yet).
- **Schema migrations as separate `.sql` files.** Today migrations live as inline C strings in `src/storage/schema.c`. Externalising them would make review and diffing easier.
- **A real content consolidation pass.** The "merge two duplicate-ish nodes into one with both authors recorded" operation is on the roadmap; today the only consolidation is by `content_hash` (i.e. byte-for-byte identical).
- **Pluggable embedding tables.** The schema assumes 1024-dim BGE-M3. Swapping to a different model (e.g. local `nomic-embed-text` 768-dim, or a 1536-dim OpenAI vector) requires a schema migration and a re-embed of every existing node. Designing that pass is open work.
- **Soft deletes.** `delete` is a hard cascade. Adding a `state = DELETED` mode (analogous to `SUPERSEDED`) would make accidental deletes recoverable for a configurable retention window.
- **Index health observability.** No way to see "how full are the FTS5 / sqlite-vec indexes? are they degrading?". A `graft stats --indexes` mode that runs `PRAGMA quick_check` and per-index stats would help with long-running deployments.
