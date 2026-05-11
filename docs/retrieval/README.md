# Retrieval

This is the heart of graft. Three orthogonal operations, each with a different shape, each tuned for a different question:

| Op | Question it answers | Cost | Returns |
| -- | ------------------- | ---- | ------- |
| `query`    | "Have I solved this before? Give me the answer with high confidence." | One-shot, ~30–80 ms warm | At most **one** verified node + signals. |
| `retrieve` | "Show me the top-k most relevant memories." | Three lists + RRF fusion | Up to `top_k` ranked nodes. |
| `explore`  | "Walk the graph from a topic, follow keyword/semantic edges, give me the connected sub-graph." | Beam search × depth | Visited nodes + traversed edges. |

The pipelines share two building blocks:

- the **embedding** (`mg_embed_text` over BGE-M3),
- the **verification stack** (`mg_verify_score` — trigram Jaccard + cosine + optional cross-encoder).

---

## 1. `query` — cache lookup with multi-signal gating

```
embed(q)
   │
   ▼
vector_topk(10)
   │
   ▼
for each candidate i (sorted by s_vec desc):
    s_lex     = trigram_jaccard(query, candidate.title)
    s_vec     = candidate.cosine
    s_jaccard = same as s_lex (kept for symmetry; CE-aware future)
    s_ce      = -1 if cross-encoder disabled
    hit_level = mg_verify_score(...)
    rank_i    = 2·(STRONG) + 1·(WEAK)
              + 0.45·s_ce + 0.25·s_lex + 0.20·s_jaccard + 0.10·s_vec
   │
   ▼
pick the best HIT (STRONG or WEAK) by rank;
otherwise: MISS + fallback_retrieve(k = query_fallback_top_k)
```

### Gating

`mg_verify_score` makes the call:

```c
strong = (CE on  ? s_ce >= cfg.strong_hit_min_ce : true)
       && s_lex  >= cfg.strong_hit_min_lex
       && s_vec  >= 0.7f;

weak   = !strong
       && s_vec  >= cfg.weak_hit_min_vec
       && s_lex  >= cfg.min_lex_overlap;
```

Default thresholds:

| Field | Default | Where |
| ----- | ------- | ----- |
| Vector sanity floor (early MISS)       | `0.30` | hard-coded |
| `s_vec` for STRONG                     | `0.70` | hard-coded |
| `s_lex` for STRONG                     | `0.15` | `cache.strong_hit_min_lex` |
| `s_vec` for WEAK                       | `0.85` | `cache.weak_hit_min_vec` |
| `s_lex` for WEAK                       | `0.05` | `cache.min_lex_overlap` |
| `s_ce` for STRONG (when CE enabled)    | `0.60` | `cache.strong_hit_min_ce` |

The `cache.weak_hit_min_vec >= STRONG vector floor` ordering is intentional: a WEAK hit requires **more** vector similarity than a STRONG hit because it gets *less* lexical / CE confirmation.

### What you actually get back

**STRONG** — agent can quote the body:

```json
{
  "hit":   "STRONG",
  "id_hex": "...",
  "title":  "...",
  "body":   "...",
  "signals": { "s_vec": 0.91, "s_lex": 0.42, "s_jaccard": 0.38, "s_ce": null }
}
```

**WEAK** — agent should treat it as a hint, not a fact. The body is `null` on purpose:

```json
{
  "hit":   "WEAK",
  "id_hex": "...",
  "title":  "...",
  "body":   null,
  "signals": { "s_vec": 0.86, "s_lex": 0.07, "s_jaccard": 0.09, "s_ce": null }
}
```

**MISS** — no candidate passed gating. The fallback gives the agent up to 5 close-but-not-good-enough neighbours so it doesn't have to re-issue a wider call:

```json
{
  "hit":  "MISS",
  "fallback_retrieve": {
    "results": [{ "id_hex": "...", "title": "...", "score": 0.014 }, ...],
    "distinct_keywords": ["..."]
  },
  "signals": { "s_vec": 0.21, ... }
}
```

### Side effects

- On STRONG: `mg_storage_touch_access(top1.id)` — bumps `access_count` and `last_access`. The `analytics` report uses these to identify "champion" nodes.
- On any non-empty `top1`: a row in `similarity_samples` with `kind=1`. Powers the `query_top1` percentiles in `graft stats`.
- `signals_only: true` (advanced; HTTP layer) suppresses both side effects, for read-only inspection.

---

## 2. `retrieve` — hybrid top-k via Reciprocal Rank Fusion

```
embed(q)
   │
   ▼
R_vec      = vector_topk(50)           (cosine on title embedding)
R_bm25_t   = fts5_search(50, title)    (BM25 over title only)
R_bm25_b   = fts5_search(50, body)     (BM25 over body only)
   │
   ▼
for each list L_i and each candidate c at rank r (1-indexed):
    rrf[c] += 1 / (k_const + r)

return top-K of rrf  (default K = retrieval.top_k = 25)
```

Why three lists, not two:

- **`R_vec`** catches paraphrases. ("HikariCP pool exhaustion" finds "connection pool starvation".)
- **`R_bm25_t`** catches exact phrasing in the **retrieval anchor**. A title-level hit is a much stronger signal than a body-level hit.
- **`R_bm25_b`** catches incidental keyword overlap in the prose. Lower-confidence than the title, but recovers cases where the body uses different wording from the title.

Score arithmetic:

- `k_const = 60` (config `retrieval.rrf_k_const`) — the standard RRF constant.
- Theoretical max per node: `3 / (60 + 1) ≈ 0.0492` (rank-1 in all three).
- Typical "good" score: `> 0.025`.
- Typical "noise floor": `< 0.005`.

`distinct_keywords` is a flat list of every keyword present on the returned nodes. Useful for the viewer's keyword chips and for the agent's `/recall` follow-ups.

### When to use `retrieve` vs `query`

- `query` when you want **one** answer with high confidence, suitable for being injected into the agent's context as a quote.
- `retrieve` when you want a small ranked list — to choose between candidates, to render a list in the UI, or to feed an LLM doing the final selection itself.

The agent integrations follow a coarse rule: **`query` first, fall back to `retrieve` on MISS**. That's what the [`recall`](../integrations/) skill does.

---

## 3. `explore` — beam search over the memory graph

```
embed(q) → q_vec
keyword filter K_set = upsert each requested keyword

seeds = vector_topk(q_vec, 2·beam_width)
seeds = filter(seeds where node has at least one KEYWORD edge with kw_id in K_set)
        (skip filter if K_set is empty)

for step in 1..depth:
    for each beam:
        candidates = neighbors(beam.tail, edges in {KEYWORD, SEMANTIC}, keyword_filter=K_set)
        skip candidates already in path
        sem_score  = clamp((cos(q_vec, dst_emb) + 1) / 2, eps, 1)
        edge_score = clamp(edge.weight, eps, 1)
        base = beam.score + log(edge_score) + alpha · log(sem_score)
                            − depth_penalty · step
        mmr  = max cos(dst_emb, m_emb)  for m in beam.path
        score' = base − mmr_lambda · mmr
    keep top-beam_width across all candidates by score'

return visited (best score per id, sorted) + traversed edges
```

Defaults (`explore.*`):

| Key | Default | Range that makes sense |
| --- | ------- | ---------------------- |
| `depth`         | 3      | 2–5 |
| `beam`          | 4      | 1–8 (1 = focused chain; 8 = wide scan) |
| `decay_gamma`   | 0.85   | 0.5–0.95 |
| `alpha`         | 0.5    | 0.3–0.8 (higher = more weight to semantic-to-query relevance) |
| `mmr_lambda`    | 0.7    | 0.5–0.9 (lower = more diverse path; higher = greedier follow) |

Output shape:

- `nodes` — every visited node, with its **best** beam score and the `depth_reached` at which it first appeared.
- `edges` — the edges the search **actually traversed**, deduped by `(src, dst, kind, keyword_id)`. Useful for the viewer to show the walk path; **not** a full subgraph dump.

### When to use `explore`

- "What do I know about X, and what's nearby?"
- "Find related decisions to this one" — feed the title of an existing node back in.
- "Walk a keyword cluster" — pass `--keyword <k>` to constrain the walk.

It is **not** a substitute for `retrieve` — beam search composes locally and can dodge a globally better answer that isn't reachable from the seeds.

---

## The verification stack

```c
mg_verify_score(ctx, query, candidate_title,
                pre_computed_s_vec, pre_computed_s_lex,
                &out);
```

Signals filled in:

| Signal | Source | Range | Meaning |
| ------ | ------ | ----- | ------- |
| `s_vec`     | `mg_storage_vector_topk`        | [-1, 1] | Cosine of query vs candidate title. |
| `s_lex`     | trigram Jaccard (`mg_text_trigram_jaccard`) | [0, 1] | Character-trigram Jaccard between query and title. |
| `s_jaccard` | same trigram Jaccard, normalised, kept separately for the future CE-aware mix | [0, 1] | Today identical to `s_lex`. |
| `s_ce`      | optional cross-encoder | [0, 1] or `-1` if disabled | BGE-reranker-v2-m3 (when wired). Today stubbed. |

The composite rank (`query_verification_rank`) is:

```
2·(STRONG) + 1·(WEAK)
 + 0.45·s_ce
 + 0.25·s_lex
 + 0.20·s_jaccard
 + 0.10·s_vec
```

The hit-level bias (`+2` / `+1`) makes the categorical gate dominate the continuous signals: a STRONG candidate always wins over a WEAK candidate, even when the WEAK candidate's continuous signals would have outscored it. Within a level, the continuous signals break ties.

---

## Calibrating thresholds for your corpus

Run `graft stats` after a few hundred nodes:

```json
"query_top1": { "p25": 0.32, "p50": 0.61, "p75": 0.84, "p90": 0.91, "p95": 0.94, "p99": 0.97 }
```

Then read the distribution:

- If `p50` is around **0.6–0.7**, the default thresholds are tuned right — most queries hit, the misses are real misses.
- If `p50 < 0.5`, the corpus is sparse or your titles are too long / off-shape. Lower `weak_hit_min_vec` toward `p50 + 0.05`, but plan a content-quality pass.
- If `p50 > 0.85`, your corpus is dense and noisy — many queries land on near-duplicates. Either consolidate (manually, for now) or raise `strong_hit_min_lex` so titles must lexically overlap before claiming a STRONG.

Re-measure after a few hundred more inserts. Threshold tuning is empirical.

---

## What's missing and how to improve it

- **Cross-encoder reranker wiring.** The stub returns `-1`; the gating already handles the disabled case correctly, but enabling a real CE would noticeably improve `s_ce` resolution on the close cases.
- **NLI contradiction detection.** `nli_enabled` exists in config; the implementation is not yet wired. The goal: when an insert is too similar to an existing node but the polarity is opposite (e.g. "X must do Y" vs "X must NOT do Y"), add a `MG_EDGE_CONTRADICTS` edge so the agent surfaces both sides.
- **Adaptive thresholds.** Today the user edits `config.yaml`. A `graft consolidate --tune` mode that proposes new thresholds based on the recent percentiles would close the loop.
- **`retrieve` per-list weighting.** RRF is unweighted across the three lists. For corpora dominated by long bodies, downweighting `R_bm25_b` would reduce noise.
- **`query` early exit on very-strong vector hits.** The current loop touches up to 10 candidates even when `s_vec > 0.95` on the first. A small short-circuit would shave a few ms on the warm hot path.
- **Distance metric switch.** Some corpora respond better to L2 than to cosine on these vectors. A config switch + the same threshold remap math would be straightforward, but is unproven on real graft corpora today.
