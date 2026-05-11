# Insert

Saving is the half of graft most users underrate. A bad insert is forever — wrong title shape, missing keywords, body that buries the real lesson — and it pollutes every future retrieval. This page is the contract: what `insert` does, what makes a good node, and how the edges are built.

## CLI

```bash
graft insert \
  --title  "Short, retrieval-shaped statement of what you learned" \
  --body   "Longer prose: the why, the trap, the workaround, a code snippet, references" \
  --keyword spring-boot --keyword validation --keyword gotcha \
  [--author "name@host"] \
  [--expires-at <unix-ms>]
```

> Aliases: `--tag` is accepted as a synonym for `--keyword`.

Response:

```json
{
  "status": 0,
  "result": {
    "id_hex":      "019e0a4466...",
    "duplicate":   false,
    "n_kw_edges":  3,
    "n_sem_edges": 2
  }
}
```

`duplicate: true` means the content hash already exists. The existing id is returned; no new node, no new edges. This is how `insert` is idempotent.

## What a "good" node looks like

The retrieval shape is the node's title. The title is the only thing that gets embedded, so it has to **be the question the future agent will ask**, not the literal name of the file or the function you were debugging.

| Bad title | Better title |
| --------- | ------------ |
| `Spring Boot validation bug` | `Spring Boot @Valid does not cascade on nested DTO fields without @Valid on the field plus @Validated on the controller` |
| `Postgres lock` | `Postgres lock_timeout=0 means wait forever, not "no lock check" — leads to silent hangs in long-running transactions` |
| `Notes about React` | `React useEffect double-fires in dev under StrictMode — wrap mutations to be idempotent, do not "fix" by removing StrictMode` |

Rules of thumb:

- Write it in the shape of the question you would ask.
- Include the **technology** word and the **specific quirk** in the title — that's what makes BGE-M3 and BM25 agree.
- The body is for the explanation, references, code snippets. The agent will only see it on a STRONG hit, never on a WEAK hit.
- Keywords are for navigation, not for retrieval — `query` does **not** filter by keywords. Use them so `explore` and `classify` work later.
- 2–4 keywords is usually right. More than 6 dilutes the keyword-edge signal.

If the user is going through `/memoryze` or `/learn` in their agent, the skill enforces this shape. If they're using the CLI directly, no one will stop a sloppy title.

---

## The insert pipeline

End-to-end, here is what the daemon does when it receives an `insert` request (`src/insert/insert.c::mg_op_insert`):

```text
1.  parse args                                  (mpack → C strings)
2.  compute content_hash                        BLAKE3(title || \0 || body || \0 || sorted_keywords)
3.  lookup by content_hash                      → duplicate path returns existing id_hex, done
4.  generate UUIDv7                             new node.id
5.  embed(title)                                → q : 1024-dim L2-normalised f32
6.  upsert each keyword                         → keyword_ids[]
7.  build edges from the embedding              src/insert/edges.c::mg_insert_build_edges_from_embedding
       7a. for each keyword in this node:
              top-k cosine restricted to nodes that already have that keyword
              add MG_EDGE_KEYWORD edge if weight >= edge_keyword_min
       7b. global vector_topk(20):
              fetch each candidate's embedding
              run greedy MMR with mmr_lambda
              add MG_EDGE_SEMANTIC edges until edge_semantic_topk or threshold
8.  open SQLite transaction
       8a. INSERT INTO nodes
       8b. INSERT INTO node_keywords
       8c. INSERT INTO node_vec (embedding)
       8d. INSERT INTO edges (kw + sem)
       8e. if supersedes:
              UPDATE old.state = SUPERSEDED
              INSERT edges (kind=SUPERSEDES, src=new, dst=old)
       8f. COMMIT
9.  record similarity_samples (one row per edge candidate, kind=0)
10. return { id_hex, n_kw_edges, n_sem_edges, duplicate=false }
```

Every step happens inside the daemon. The CLI sees a single request / response.

---

## Edge construction (`src/insert/edges.c`)

Two kinds of edges are built at insert time:

### Keyword edges (`MG_EDGE_KEYWORD`)

For each keyword on the new node:

```
top = vector_topk_by_keyword(q, keyword_id, k = edge_keyword_topk)
for each candidate c in top:
    if cos(c) >= edge_keyword_min:
        add edge (new, c, kind=KEYWORD, keyword_id, weight=cos)
```

Defaults: `edge_keyword_topk = 5`, `edge_keyword_min = 0.5`.

The intent is "find the K most-similar nodes that share **this** keyword, and link them by it". This is what makes keyword chips in the viewer meaningful — a click on a keyword chip lights up the actual cluster, not all nodes that happen to mention the word once.

### Semantic edges (`MG_EDGE_SEMANTIC`)

Globally over the corpus:

```
top = vector_topk(q, k = edge_semantic_topk)
pool = top minus the source node
selected = []
while |selected| < edge_semantic_topk and pool not empty:
    pick c maximising  lambda · cos(q, c) − (1 − lambda) · max(cos(c, s)  for s in selected)
    if cos(q, c) < edge_semantic_min: stop
    move c from pool to selected
emit edges (new, s, kind=SEMANTIC, weight=cos) for each s in selected
```

Defaults: `edge_semantic_topk = 20`, `edge_semantic_min = 0.6`, `mmr_lambda = 0.7`.

**MMR (Maximal Marginal Relevance)** is critical here. Without it, a new node tends to get linked to ten near-duplicates of the same neighbour. MMR balances "highly relevant to me" against "I haven't picked anything like this yet". The result: edges spread the new node across a diverse set of related nodes, not a cluster of one.

---

## Idempotency and content hash

The hash is `BLAKE3(title || '\0' || body || '\0' || sorted_keywords joined by ',')`.

What's in the hash:

- `title`
- `body`
- `keywords`, **sorted lexicographically** (so the same node with `[a, b]` and `[b, a]` hashes identically)

What's deliberately **not** in the hash:

- `author` — same memory saved by different users still dedups.
- `created_at` / `expires_at` — same memory saved at different times still dedups.
- `supersedes` — the supersession relation is separate from content identity.

If you need to retain "who saved this", encode it in a keyword (`#by-andrea`) or in the body. If you need version history that does NOT dedup, change the body even slightly — the hash will diverge.

`mg_storage_node_id_by_hash` is one indexed lookup. Fast on any corpus size.

---

## Supersession (atomic edits)

When the request includes `supersedes: <old_id_hex>` (CLI flag and HTTP body both support this):

1. The new node is inserted normally.
2. In the same SQLite transaction: `UPDATE nodes SET state = MG_NODE_SUPERSEDED WHERE id = <old_id>`.
3. In the same transaction: `INSERT INTO edges (src=new, dst=old, kind=MG_EDGE_SUPERSEDES, weight=1.0)`.

After commit:

- The new id is the canonical retrieval target.
- The old id stays reachable via `graft get` (or `GET /v1/nodes/{old_id}`), so history is preserved.
- `query`, `retrieve`, `explore` filter `state != ACTIVE` out — agents will not see the old node again unless they ask for it by id.
- The viewer renders superseded nodes in muted gray with red `SUPERSEDES` edges so the chain is visually obvious.

This is the mechanism behind **click-to-edit** in the 3D viewer.

---

## Classify (`graft classify --title "..."`)

A small but useful pre-insert helper. Given a draft title:

```
embed(title)
top = vector_topk(50)
for each n in top:
    walk MG_EDGE_KEYWORD neighbours → collect keyword_ids
counts = histogram of keyword_ids
sort counts desc, break ties by keyword text
return top 15 keyword texts as suggested_keywords
```

In other words: "what keywords do nodes-like-this-one already carry?". The result is a list, not a prediction — `classify` does **not** generate new keywords. If the graph is empty, the suggestion list is empty.

Use it in your insertion script when the user doesn't pass `--keyword` explicitly. The `/memoryze` skill does this automatically.

---

## What `insert` does **not** do

- It does not call out to an LLM. Classification is graph-based, not generative.
- It does not normalise the body content (no Markdown linting, no link checking). What you give it is what you get back.
- It does not deduplicate on similarity. Two near-duplicate inserts (different titles, same body up to whitespace) will both go in — by design, until manual `consolidate` is in place. They will of course link to each other via SEMANTIC edges, so `explore` keeps the cluster visible.
- It does not validate keywords for spelling. `springboot`, `spring-boot`, and `spring_boot` are three distinct keywords. Be consistent.

---

## What's missing and how to improve it

- **`graft insert --from <file>`** for batch ingestion of NDJSON or Markdown-with-frontmatter. Today batch ingestion runs through N CLI subprocesses (one per node), which pays the socket setup cost N times. A streaming op would be much faster.
- **Optional NLI check at insert time.** When `nli_enabled` is wired, the insert path could compare the new node against its top semantic neighbour and emit a `MG_EDGE_CONTRADICTS` edge when the polarity flips.
- **Title-quality lint.** A small heuristic ("title is one word", "title has fewer than three content tokens", "title ends with a question mark") could print a warning before saving. Off by default, opt-in via `--strict`.
- **Author override hardening.** Right now `GRAFT_AUTHOR=""` opts out; `GRAFT_AUTHOR` unset uses `<user>@<host>`. There's no way to require an explicit author (e.g. for shared profiles). A config flag would close that gap.
- **Sub-second body diff** for an UPDATE-vs-SUPERSEDE choice. Today the client decides; the daemon never offers a "did you mean to supersede?" hint. A `--smart-supersede` mode that calls `query` first and proposes supersession on a STRONG hit would reduce duplicate inserts.
