# Embeddings

Graft turns text into 1024-dimensional vectors with **BGE-M3** running on a tightly-scoped fork of **llama.cpp**. The vectors are L2-normalised at write time and stored verbatim in `sqlite-vec`.

This document covers:

- which model graft uses and why,
- how to control threads / context size,
- CPU vs CUDA vs ROCm,
- what to do when embeddings look "off".

The C surface is in [`include/graft/embed.h`](../../include/graft/embed.h); the implementation in [`src/embed/embed.c`](../../src/embed/embed.c) and the llama backend lifecycle in [`src/common/llama_backend.c`](../../src/common/llama_backend.c).

---

## The model

| Property | Value |
| -------- | ----- |
| Family | BGE-M3 (BAAI General Embeddings, multilingual) |
| Dimensions | **1024** |
| Quantisation | **Q8_0** GGUF |
| File on disk | `models/bge-m3.gguf` (~600 MB) |
| Source | `huggingface.co/lm-kit/bge-m3-gguf` |
| Pin (Homebrew) | a specific commit with SHA256, see `Formula/graft.rb` |
| L2-normalised output | yes, by the daemon (`mg_embed_text`) |

Why BGE-M3:

- **Multilingual out of the box** — the same vector space across English, Italian, Spanish, German, Chinese, etc. The unit tests include cross-lingual queries.
- **Small and fast on CPU** — under 100 ms per query on a modern laptop with 4 threads. Q8_0 keeps the on-disk and in-memory footprint manageable.
- **Stable cosine geometry** — the score distribution is consistent across sentence lengths, which makes the threshold-based gating in `mg_verify_score` predictable.
- **Permissive licence** (MIT) on the model itself; the GGUF re-release at `lm-kit/bge-m3-gguf` is also redistributable.

## Cosine math

Vectors are L2-normalised on the way in. That means **cosine similarity = dot product**, computed in `mg_cosine`:

```c
float mg_cosine(const mg_embedding_t a, const mg_embedding_t b) {
    float s = 0.f;
    for (int i = 0; i < MG_EMBEDDING_DIM; i++) s += a[i] * b[i];
    return s;
}
```

Range:

- `+1.0` — same direction (semantically nearly identical).
- ` 0.0` — orthogonal.
- `-1.0` — opposite direction (rare for natural text; BGE-M3 tends to keep similar topics in the same half-space).

The thresholds in `cache.*` and `edges.*` are all in cosine units, so values like `0.7` and `0.85` have intuitive meaning ("strong match" and "very strong match" respectively for BGE-M3).

## Configuration

Knobs in `config.yaml`:

```yaml
embedding:
  model_path:      "./models/bge-m3.gguf"  # absolute path on Homebrew builds
  threads:         4
  ctx_size:        8192
  hardware_accel:  false
```

| Key | Effect |
| --- | ------ |
| `model_path` | Where to load the GGUF from. The Homebrew formula rewrites this to the Cellar location at install time. |
| `threads` | OpenMP-style CPU pool used by llama.cpp. **More threads is not always faster** — graft's per-call workload is small. 4 is a strong default for laptops. Go up to your physical-core count if you're running concurrent clients (e.g. an integration on top of `/v1/match`). |
| `ctx_size` | Context window the embedding model is configured with. BGE-M3 honours up to ~8 K tokens; oversized titles are truncated by the tokenizer. |
| `hardware_accel` | Offload all layers to GPU. **Requires** llama.cpp to have been built with `-DGGML_CUDA=ON` or `-DGGML_HIP=ON`. See "GPU builds" below. |

## CPU performance baseline

Measured on a 2024 laptop (M2 Pro, macOS) with the default config:

| Operation               | Median latency | Notes |
| ----------------------- | -------------- | ----- |
| Embed a 50-token title  | ~38 ms         | Dominated by tokenizer + matmul. |
| Embed a 500-token body  | ~75 ms         | Linear-ish in tokens. |
| `mg_cosine` × 1000      | <1 µs          | Tight loop over 1024 floats. |
| `mg_storage_vector_topk` over 10 K nodes | ~3 ms | sqlite-vec brute force, no ANN yet. |

The whole `query` round-trip — embed + vector top-1 + verify + write JSON — comes in at **30–80 ms** warm.

## GPU builds

The installer builds with GPU support when you pass `GRAFT_GPU=cuda` or `GRAFT_GPU=hip`. Internally that's `cmake -DGGML_CUDA=ON` or `-DGGML_HIP=ON` on the llama.cpp subbuild.

Once built with GPU support:

```yaml
embedding:
  hardware_accel: true
```

What you get:

- **Cold start dominated by the GPU copy** of the model weights — adds ~600 MB of VRAM consumption.
- **Per-embedding latency drops to single-digit ms** on consumer GPUs.
- **Concurrent throughput scales** beyond what CPU can sustain — useful when graft is the L2/L3 layer behind a busy L1 Redis (see [`microservices/`](../microservices/)).

What you do not get:

- **No silent fallback.** The daemon refuses to start with `hardware_accel: true` on a CPU-only build, and refuses to start if CUDA / HIP backend init fails at runtime. The error in the daemon log explicitly tells you to rebuild with the right flag. Silent fallback hides regressions that are very expensive to debug later.

### Backend lifecycle

`mg_embed_init` calls `mg_llama_backend_acquire`, which:

- initialises the global llama.cpp backend exactly once,
- loads the model with the requested context size,
- offloads all layers to GPU when `hardware_accel` is true,
- returns an `mg_embed_ctx_t` the daemon keeps for its lifetime.

On shutdown, `mg_embed_shutdown` frees the model and `mg_llama_backend_release` decrements the reference count; the global backend is freed when the last context goes away.

## How embeddings are used downstream

| Caller | What it does with the embedding |
| ------ | --------------------------------- |
| **insert** | Stores the **title's** embedding in `node_vec`. Computes per-keyword and global top-k to build keyword/semantic edges via MMR. |
| **query**  | Embeds the input, runs `vector_topk(10)`, picks the best candidate via the verify pipeline, gates STRONG / WEAK / MISS. |
| **retrieve** | Same as `query` but keeps top-50 from the vector list and fuses with FTS5 BM25 via RRF. |
| **explore** | Seeds the beam, scores each step's "how relevant to the query" signal at traversal time. |
| **classify** | `vector_topk(50)` on the **title**, then counts the keywords reachable through KEYWORD edges of those nearest neighbours. |

Note that graft **embeds the title, not the body**. The title is the retrieval anchor. The body adds depth (and lexical signal via BM25), but does not get its own vector. This is a deliberate trade-off:

- titles are short → cheaper to embed,
- BGE-M3 handles short queries well (the typical agent question is a sentence),
- it forces users to write retrieval-shaped titles (the actual "what does this memory answer?" question).

If you violate this — e.g. paste a paragraph into the title — retrieval suffers. The skills (`/memoryze`, `/learn`) enforce title shape on the way in.

## Troubleshooting

| Symptom | Likely cause |
| ------- | ------------ |
| `embed init failed: MG_ERR_CONFIG` | `hardware_accel: true` but the binary was built CPU-only. Rebuild with `GRAFT_GPU=cuda` (or `hip`), or set `hardware_accel: false`. |
| `embed init failed: MG_ERR_IO` | Model file not found or unreadable. Check `embedding.model_path` and the BGE-M3 download. |
| Very high cold-start latency | First model load on a cold OS page cache. The second invocation is usually <500 ms. |
| All queries MISS even on the exact phrase | The model isn't loaded (check the daemon stderr / `~/.graft/memgraphd.err.log`). |
| Score distribution very narrow (`p25 ≈ p99`) | The corpus is dominated by near-duplicates, or every node uses the same title shape. Run `graft consolidate` to inspect, and rewrite the titles to be more retrieval-shaped. |

---

## What's missing and how to improve it

- **Pluggable embedding backends.** The implementation assumes BGE-M3 via llama.cpp. The function pointers in `mg_embed_ctx_t` would need to be virtualised to support a second backend (e.g. an OpenAI client, or a `nomic-embed-text` GGUF). Doing this would also need a per-row "which model generated this vector" tag in `node_vec` so a mixed-corpus is safe.
- **Cross-encoder reranker.** `cross_encoder_enabled` exists in config; `src/verify/crossencoder.c` is a wired-up stub returning `-1` from `mg_ce_score_pair`. Hooking up BGE-reranker-v2-m3 as a second GGUF model would close the loop and let `cache.strong_hit_min_ce` actually gate.
- **Approximate top-k**. `sqlite-vec` currently does brute-force scoring. For graphs above ~50 K nodes a proper ANN index (HNSW, IVF-PQ) would matter. Out of scope for now — the typical agent graph is much smaller — but a clean place to swap in a different vector backend.
- **Per-language tokenizer hints.** BGE-M3 handles Italian / English well, but corpora dominated by source code or extremely short titles (1–2 words) tend to bunch up in the score distribution. A small input normaliser (lowercase code, strip surrounding quotes, fold CamelCase) would tighten the distribution.
- **Embedding cache for repeat texts**. The same query embedded twice in a row is computed twice. A tiny LRU keyed by `hash(text)` would shave the cost for high-fan-out workloads.
