# Benchmark

A small, reproducible benchmark of what graft promises: that `graft query`
returns `STRONG` only when it has the right answer, that `graft retrieve`
ranks the right node first, and how long both take.

```bash
python bench/run.py                       # uses the graft on PATH
python bench/run.py --graft build/graft   # a local build
```

The script starts its **own** `graftd` with a temporary `GRAFT_HOME`, socket,
database and usage log, and an installer-style config that sets only the model
path, so every other setting is the built-in default. Your graph and your
`usage.jsonl` are never touched. It needs Python 3.10+ (standard library only)
and the BGE-M3 model (`--model`, default `$GRAFT_HOME/models/bge-m3.gguf`).

On Windows, a development build needs the llama.cpp and MinGW DLLs on `PATH`
first: `third_party/llama.cpp/build/bin` and `C:\msys64\mingw64\bin`.

## Corpus

[`corpus/nodes.jsonl`](./corpus/nodes.jsonl) holds 50 coding-agent memories
(one gotcha and its fix each, across git, Docker, PostgreSQL, Python, Node,
React, Kubernetes, CI and more). [`corpus/queries.jsonl`](./corpus/queries.jsonl)
holds the labelled questions. Each query has one of four kinds:

| Kind | What it tests |
| ---- | ------------- |
| `exact` | The node title verbatim. Generated from `nodes.jsonl`, not stored. |
| `paraphrase` | The same problem in different English words. |
| `crosslang` | The same problem asked in Italian against an English node. |
| `negative` | No node answers it. Most are on the same technologies as the corpus, which is the hard case for a cache. |

`expect` is the `key` of the node that answers the query, or `null`.

## Metrics

- **STRONG precision**: share of `STRONG` answers that point at the expected
  node. This is the number the verified cache stands on.
- **False STRONG on negatives**: share of unanswerable queries that still got
  a `STRONG`.
- **STRONG hit rate**: share of answerable queries answered `STRONG` with the
  right node.
- **recall@1 / recall@k / MRR** for `graft retrieve`.
- **Latency**: p50 / p95 of the CLI-to-daemon round trip, taken from the run's
  private usage log, plus the p50 including process start-up.

Each run writes `results/<date>-<os>-<version>.json` (summary plus every query
with its signals, so thresholds can be studied offline) and a `.md` table.

## Caveats

- 50 nodes is a small graph. It measures the verifier's decisions, not
  behaviour at 10k nodes.
- The corpus was written for this benchmark. Tuning thresholds on it and then
  quoting the same numbers would overfit; tune on a held-out query set.
- Latency depends on the CPU and the embedding thread count. Compare runs
  from the same machine only.
