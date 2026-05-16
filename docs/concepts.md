# Graft — Core Concepts

This page explains the fundamental ideas behind graft in plain language. No C internals, no SQLite schema — just the mental model you need to use graft effectively.

---

## Memory node

The **memory node** is the basic unit of storage in graft.

A node has:
- **`title`** — a short, retrieval-shaped statement of what you learned. This is what the semantic search matches against. Think of it as the question that this node answers, phrased as an answer.
- **`body`** — the full context: the why, the trap, the workaround, the decision rationale. This is what gets injected into the agent's context on a STRONG hit.
- **`keywords`** — one or more tags that link this node into the graph (e.g. `spring-boot`, `validation`, `gotcha`). Keywords create edges between related nodes.
- **`author`** — who (or which agent) created this node. Defaults to `user@hostname`.
- **`expires_at`** — optional TTL. After this timestamp, the node is hidden from all queries and cleaned up by `graft consolidate`.
- **`state`** — `ACTIVE`, `STALE`, or `SUPERSEDED`. Only ACTIVE nodes are returned by queries.

**Writing good titles is the most important thing.** The title is the retrieval anchor. A good title is the specific, complete sentence that someone would be looking for when they need this memory. A vague title (`"Spring Boot issue"`) produces weak hits; a specific one (`"Spring Boot @Valid does not cascade to nested DTOs without @Valid on the nested field"`) produces STRONG hits even with different query phrasing.

---

## Profile

A **profile** is a fully isolated memory space. Each profile has its own:
- SQLite database (no data sharing between profiles)
- daemon instance (separate process, separate socket)
- configuration (can override `config.yaml` per profile)

The active profile is determined by the `GRAFT_PROFILE` environment variable (default: `default`).

```bash
graft profile list                  # see all profiles
graft profile add work              # create a new one
eval "$(graft profile set work)"    # switch in current shell (bash/zsh)
graft profile set work | iex        # switch in PowerShell
```

**When to use multiple profiles:**
- `default` — personal learning and general-purpose memory
- `work` — project-specific decisions, employer-specific conventions
- `project-x` — memory scoped to a single project or repository

Profiles are isolated by design. If you want to share knowledge across profiles, use `graft profile merge` or `graft profile export` / `import`.

---

## Semantic cache

The **semantic cache** is what `graft query` implements.

When you call `graft query "some text"`:
1. Graft embeds the query text using BGE-M3 (1024-dimensional vector).
2. It finds the top-1 closest node by cosine similarity in the vector index.
3. It runs a **verify step** — combining trigram-Jaccard lexical similarity and the cosine score — to decide if the hit is reliable.
4. It returns one of three verdicts:
   - **STRONG** — both semantic and lexical signals pass the gate. High confidence. Returns `title` + `body`.
   - **WEAK** — semantic signal passes but lexical is below threshold. Returns `title` only (body may be off-topic).
   - **MISS** — neither signal is strong enough. Returns fallback results from `retrieve`.

The verify step is what distinguishes graft from a plain vector search: it **refuses to hallucinate a match**. A low-confidence cosine hit does not become a STRONG response.

---

## Graph edge

Graft does not store memories as isolated items — it connects them into a **graph**.

There are four edge types:

| Type | Created by | Meaning |
| ---- | ---------- | ------- |
| `KEYWORD` | Insert pipeline | Two nodes share a keyword (e.g. both tagged `spring-boot`) |
| `SEMANTIC` | Insert pipeline | Two nodes are semantically similar (cosine above threshold) |
| `SUPERSEDES` | `insert --supersedes` | New node replaces an older one |
| `CONTRADICTS` | NLI pipeline (planned) | New node contradicts an existing one |

KEYWORD and SEMANTIC edges are **bidirectional at traversal time**: when you call `graft explore`, edges are walked in both directions regardless of which node was inserted first.

The graph enables **graph walks** via `graft explore`, which follows edges with beam search and MMR diversity — useful for "what does graft know about topic X and related topics?" rather than the single-best-match lookup of `graft query`.

---

## Supersession

**Supersession** is how graft handles outdated knowledge without losing history.

When you learn that a previously-stored fix is wrong, or that a decision has changed:

```bash
# Find the old node
graft retrieve "the thing that changed"
# Note the id_hex from the result

# Insert the corrected version, linking it to the old one
graft insert \
  --title "Corrected: ..." \
  --body  "Updated fix: ..." \
  --keyword ...
  # Then supersede the old node via the HTTP API or a future CLI flag
```

The old node moves to state `SUPERSEDED`. It is no longer returned by queries, but it remains in the database for audit purposes. The `SUPERSEDES` edge from the new node to the old one makes the history traversable.

This is safer than `graft delete` for knowledge that changed — deletion loses the history entirely.

---

## Confidence

**Confidence** is not a single number in graft — it is expressed as the hit level: STRONG, WEAK, or MISS.

The verify pipeline combines:
- **`s_vec`** — cosine similarity between query and node embeddings (0–1).
- **`s_lex`** — trigram-Jaccard overlap between query text and node title (0–1).
- **`s_ce`** — cross-encoder score (0–1, optional — requires `verification.cross_encoder_enabled: true` and a cross-encoder model).

The gate logic (default, non-fused mode):
- **STRONG** if `s_vec ≥ 0.75` AND `s_lex ≥ 0.15` (both signals agree).
- **STRONG** if `s_vec ≥ 0.85` alone (semantic signal is very high, lexical not needed).
- **WEAK** if `s_vec ≥ 0.85` but `s_lex < 0.15` (high vector, low lexical).
- **MISS** otherwise.

All thresholds are configurable in `config.yaml` under the `cache:` section.

Check `graft stats` to see the distribution of your actual similarity scores and calibrate thresholds to your corpus.

---

## Local-first

**Local-first** means graft runs entirely on your machine.

- The embedding model (BGE-M3, ~600 MB) runs locally via llama.cpp.
- The database is a single SQLite file on disk.
- The daemon communicates over an AF_UNIX socket (Windows: named pipe).
- No data ever leaves your machine unless you explicitly configure remote profiles.

The tradeoff: the first cold-start of the daemon takes 1–2 seconds while llama.cpp loads the model. After that, queries are fast (tens of milliseconds warm).

GPU acceleration is opt-in: set `hardware_accel: true` in `config.yaml` and build with `GRAFT_GPU=cuda` or `GRAFT_GPU=hip`.

---

## Further reading

- [Use cases](./use-cases.md) — concrete scenarios for each concept above
- [CLI reference](./cli/) — every command and flag
- [Retrieval](./retrieval/) — deep dive on `query`, `retrieve`, `explore`, and the verify pipeline
- [Insert](./insert/) — how nodes and edges are built
- [Profiles](./profiles/) — multi-tenancy, export, import, remote sync
- [Configuration](./configuration/) — all `config.yaml` keys and environment variables

**What's missing and how to improve it**

- Fused-gate mode (`verify_use_fused_gate: true`) is not yet documented here — add an explanation of how the weighted fusion score replaces the boolean STRONG/WEAK rules.
- Cross-encoder confidence is not yet documented — add once the CE is wired (roadmap item).
- The NLI / CONTRADICTS pipeline is planned but not yet implemented — document once available.
