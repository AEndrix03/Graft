# Graft as memory infrastructure for microservices

This page is the **one** you should read if you're thinking about putting graft behind your services. It is the canonical pattern.

> Recommended stack:
> - **L1 — Redis**           — millisecond cache, exact-key reuse, short TTL.
> - **L2 — graft (semantic)** — milli-to-tens-of-ms verified semantic cache. Where graft really shines.
> - **L3 — graft + AI agent** — full agentic retrieve and reasoning on a miss; final fallback.

Each layer has a different question shape and a different cost. Plumbing all three is what makes the system feel "instant most of the time, smart when it has to be".

---

## The L1 / L2 / L3 story

```
Client
  │
  │     ┌──────────────────────────────┐
  │     │ L1 — Redis                   │  ← exact-key hits, ~1 ms
  │     │  GET cache:<sha256(prompt)>  │
  │     └─────────────┬────────────────┘
  │            MISS   │
  │                   ▼
  │     ┌──────────────────────────────┐
  │     │ L2 — graft semantic cache    │  ← paraphrase-aware, ~30–80 ms
  │     │  GET /v1/match?text=...      │
  │     │   → STRONG / WEAK / MISS     │
  │     └─────────────┬────────────────┘
  │            MISS   │
  │                   ▼
  │     ┌──────────────────────────────┐
  │     │ L3 — agentic retrieve        │  ← graft retrieve + LLM, ~500 ms–N s
  │     │  GET /v1/search?top_k=K      │
  │     │   → feed top-K to your LLM   │
  │     │   → LLM synthesises answer   │
  │     │   → POST /v1/insert  (write back as new memory)
  │     └─────────────┬────────────────┘
  │                   ▼
  │     ┌──────────────────────────────┐
  │     │ Write back through L1        │  next-time fast path
  │     │  SET cache:<sha256(prompt)>  │
  │     └──────────────────────────────┘
  ▼
Response
```

The whole point is that **most requests land in L1 or L2** and never reach the LLM (L3). Costs and latencies look like:

| Layer | Latency | Cost / request | What it answers |
| ----- | ------- | -------------- | --------------- |
| L1 Redis           | ~1 ms      | RAM bytes              | "Have we seen *this exact prompt* before?" |
| L2 graft semantic  | ~30–80 ms  | CPU (~free)            | "Have we seen *a question that means this* before?" |
| L3 graft + LLM     | ~500 ms–N s| LLM tokens             | "We have not. Let me reason from related memories." |

---

## Why each layer matters

### L1 — Redis exact cache

The cheapest possible win. If a user (or another service) asks the **literal same** question we just answered, we return the cached body in ~1 ms. No embedding, no SQL, no LLM. Use a normalised key:

```python
def l1_key(prompt: str) -> str:
    norm = prompt.strip().lower()
    return "cache:" + hashlib.sha256(norm.encode("utf-8")).hexdigest()
```

This is the fastest layer. It is also the dumbest layer — it does not survive any reformulation. **"How do I cascade @Valid in Spring Boot?"** and **"why does @Valid not work on nested DTOs?"** are *different* in L1.

TTL on L1 should be short enough that you don't accumulate stale answers (e.g. 1 h to 24 h). Failure mode: a miss falls through to L2.

### L2 — graft semantic cache

This is where graft earns its keep.

`GET /v1/match?text=<prompt>` does:

1. Embed `<prompt>` with BGE-M3 (~30 ms).
2. Top-1 vector lookup in the graph.
3. Verify the candidate against the prompt via trigram Jaccard + cosine + optional cross-encoder.
4. Return `STRONG` / `WEAK` / `MISS`.

On `STRONG`, you can return the candidate's body to the caller **directly**. The verification step protects you from "vector said 0.85 cosine but the actual phrasing was unrelated" — the trigram Jaccard gate is what makes the cache safe.

On `WEAK`, you have a choice:

- treat it as a hint and proceed to L3 anyway, or
- return the title alone (the body is intentionally `null` on WEAK), letting your service decide.

On `MISS`, graft also returns a small `fallback_retrieve` list of close-but-not-good-enough neighbours, so when you fall through to L3 you can feed those in as context **for free** — no extra round trip.

> **Why graft instead of "embeddings table in Postgres + cosine"?** The interesting work is not the vector lookup — that's two SQL statements anywhere. The interesting work is the **gating** (`mg_verify_score`) that decides "this hit is safe to quote" vs "this hit is suggestive, do not quote". Graft does that gating consistently, with thresholds you can tune from a `stats` percentile dump.

### L3 — agentic retrieve

When L2 misses, your service should:

1. Call `GET /v1/search?text=<prompt>&top_k=K` to get the top-K neighbours.
2. Feed them to your LLM as context.
3. Let the LLM synthesise the final answer.
4. **Write the answer back to graft** with `POST /v1/insert`, so the next caller hits L2 STRONG instead.

```python
results = httpx.get(f"{GRAFT}/v1/search", params={"text": prompt, "top_k": 8}).json()
context = render_for_llm(results["result"]["results"])
answer  = llm.chat(prompt, context)

# Write back so this is a STRONG L2 hit next time.
httpx.post(f"{GRAFT}/v1/insert", json={
    "title":    prompt,                      # retrieval anchor
    "body":     answer,                      # the actual answer
    "keywords": classify_keywords(prompt),   # GET /v1/classify gives a starting set
})
```

This is the loop that lets the system **get smarter over time** without any manual ops work. Every L3 hit reduces the population of L3 hits for the next thousand requests.

---

## Where graft fits cleanly

- **Internal LLM tooling**. Replace the "let's just call OpenAI again" pattern with "L1 → L2 → L3" and you cut tokens by 60–90 % within a few weeks. The savings compound.
- **Support / FAQ pipelines**. Every resolved ticket becomes a node. Future similar tickets hit L2 STRONG.
- **Internal documentation answering**. Combine `/learn` (batch-ingest your docs) with the L1/L2/L3 stack and your "ask the docs" feature is essentially free at runtime.
- **Agent platforms**. Each agent has its own profile (`tenant`); each tenant's graph stays isolated; multi-tenant SaaS becomes a checkbox.

## Where graft is the wrong tool

- **You need durable transactional storage for business records.** Graft is a memory layer, not a system of record. Use it next to Postgres, not instead of it.
- **You need write throughput in the thousands per second.** The SQLite-backed write path tops out in the hundreds. Read paths are happy at much higher rates.
- **Your "memories" are large structured documents you want to search by attribute, not by meaning.** That's a full-text + structured search problem, and Elastic or Meili will serve you better.

---

## Reference deployment shapes

### Single-machine, single-tenant

The simplest case. Useful for personal tools and small teams.

```text
[your service] ──► localhost L1 (Redis) ──► localhost L2 (graftd at 127.0.0.1:9977) ──► local LLM
                                                                                       (Ollama, llama.cpp, etc.)
```

Nothing leaves the machine. Backup is `cp graft.db dest/`.

### Single-machine, multi-tenant via profiles

Add a `tenant_id` header at the gateway level. Map each tenant to a graft profile. The gateway sets `GRAFT_PROFILE=<tenant>` in the env of the `graft` invocations (or, in the all-HTTP path, runs N daemons each on its own port).

### Production: gateway + per-region daemon

```text
[users]
   │
   ▼
[Caddy / nginx / Cloudflare]  TLS termination
   │
   ▼
[OAuth gateway]               integrations/mcp-server/oauth_gateway.py
   │                          validates OIDC, enforces scopes
   ▼
[graftd 127.0.0.1:9977]       local-first, never directly exposed
   │
   └── SQLite + sqlite-vec + FTS5 on encrypted volume
```

Pair with:

- L1 Redis cluster (regional) keyed on `(tenant_id, sha256(prompt))`.
- LLM provider behind a budget-aware caller.

---

## End-to-end pseudocode

```python
import hashlib, json, redis, httpx

r = redis.Redis()
GRAFT = "http://127.0.0.1:9977"

def l1_get(prompt):
    return r.get("cache:" + hashlib.sha256(prompt.encode()).hexdigest())

def l1_set(prompt, body, ttl=3600):
    r.set("cache:" + hashlib.sha256(prompt.encode()).hexdigest(), body, ex=ttl)

def l2_match(prompt):
    resp = httpx.get(f"{GRAFT}/v1/match", params={"text": prompt}).json()
    return resp["result"]

def l3_answer(prompt):
    search = httpx.get(f"{GRAFT}/v1/search",
                       params={"text": prompt, "top_k": 8}).json()
    context = render_for_llm(search["result"]["results"])
    answer  = call_llm(prompt, context)

    httpx.post(f"{GRAFT}/v1/insert", json={
        "title":    prompt,
        "body":     answer,
        "keywords": classify_keywords(prompt),
    })
    return answer

def answer(prompt):
    # L1
    if (cached := l1_get(prompt)):
        return cached.decode("utf-8")

    # L2
    m = l2_match(prompt)
    if m["hit"] == "STRONG":
        body = m["body"]
        l1_set(prompt, body)
        return body

    # L3
    body = l3_answer(prompt)
    l1_set(prompt, body)
    return body
```

This is the whole pattern. Everything else is operational shading (rate limits, tenant isolation, observability, budget caps), not architectural difference.

---

## Practical thresholds and observability

- Tune your `cache.*` thresholds from a real corpus. Run `graft stats` after a few hundred inserts, look at `query_top1` percentiles, and pick:
  - `weak_hit_min_vec` around the `p50`,
  - `strong_hit_min_lex` so STRONG hits actually share trigrams with the title.
- Watch the **hit ratio** with `graft analytics`. A healthy production graph runs at 60–80 % STRONG hits on the L2 layer.
- Watch the **distribution** of `query_top1`. If it bunches near `0.9+`, the corpus is too dense (consolidate); if it spreads broadly toward `0.3`, the corpus is too sparse (ingest more).
- The L1 hit ratio should be reasonable too — if it's tiny, your prompts vary too much for an exact cache (which means L2 is doing the heavy lifting).

---

## Failure modes and how to handle them

| Failure | Behaviour | Mitigation |
| ------- | --------- | ---------- |
| `graftd` not reachable | The HTTP client gets a connection refused. | Fall back through to L3 directly. The agent should treat graft as a cache, not a system of record. |
| Daemon healthy but slow (cold start) | First call takes 1–2 s. | Run a warm-up loop in the service's startup (`GET /v1/healthz` then `GET /v1/match?text=warmup`). |
| Wrong threshold (too many STRONG that are actually wrong) | Users complain about confidently-wrong answers. | Raise `strong_hit_min_lex` and possibly enable the cross-encoder. |
| Wrong threshold (too many MISS) | L3 / LLM cost balloons. | Lower `weak_hit_min_vec`; verify there's enough corpus to find anything. |
| Hot keyword drift | A few keywords accumulate most of the edges and the graph becomes "starlike". | Run `graft consolidate` and review the report; rename or split the dominant keywords. |
| Disk full | SQLite refuses to write, daemon returns errors on `insert`. | Standard ops practice; nothing graft-specific. The WAL recovery is automatic on restart. |

---

## What's missing and how to improve it

- **A reference deployment Helm chart / Compose file**. Today you read this page and write your own. A `deploy/` directory with a working `docker-compose.yml` (graftd + Redis + OAuth gateway + a tiny demo backend) would lower the barrier to "try it for real".
- **A `graft proxy` mode** that owns the L1 Redis + L2 daemon negotiation on behalf of callers. Today the L1 fast path lives in the calling service; a thin reverse-proxy mode that did it for you would simplify integrations.
- **Per-route hit-rate metrics** exposed by the daemon. Prometheus `/metrics` endpoint with `graft_query_hit_total{level="STRONG|WEAK|MISS"}` etc. would unlock standard dashboarding.
- **Write-back rate-limiting**. The "L3 answers, then writes to L2" loop is unbounded today. A bounded queue with backpressure would help on bursty workloads.
- **Idempotency keys at the HTTP layer**. The CLI deduplicates on `content_hash`; the HTTP `POST /v1/insert` does too, but it doesn't expose a request-level idempotency key for retried writes.
- **Cross-tenant search policy**. There is no built-in "search tenant A's graph but only the public part". Today you separate via profiles; a finer-grained access model is open work.
