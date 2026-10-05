# FTS token joining: implicit AND vs OR — 2026-10-05

Evaluation for [#21](https://github.com/AEndrix03/Graft/issues/21): should the
scoped full-text queries of `graft retrieve` join the query tokens with OR
instead of FTS5's implicit AND?

**Decision: keep implicit AND.** OR revives the lexical branch, but end to end it
makes `retrieve` worse on both the held-out and the dev set, so it fails the
issue's adoption rule.

graft 0.2.0 (develop), Windows, built-in defaults, BGE-M3, the bench corpus
(65 notes). The baseline (`and`) reproduces the published 0.1.1 numbers exactly.

## Only `retrieve` is affected

`mg_storage_fts_search` is used by `retrieve` only (the two BM25 lists of its
RRF fusion), and by `query` only through the MISS fallback list. `query`
gating uses vectors, a trigram Jaccard and the verifier: its STRONG precision,
hit rate and false-STRONG rate on negatives were identical for every strategy.

## The lexical branch alone ([`fts_lexical.py`](../../fts_lexical.py))

With implicit AND, the title and body lists are **empty for 90/90 held-out and
99/100 dev questions**: a natural-language question always holds some word that
the note lacks. Today `retrieve` is in practice vector-only, with BM25 helping
only on near-verbatim titles. With OR the lists come alive:

| Set | Strategy | List | empty | recall@1 | MRR | non-empty on negatives |
| --- | -------- | ---- | -: | -: | -: | -: |
| heldout | and | title | 90/90 | 0.000 | 0.000 | 0/30 |
| heldout | or | title | 0/90 | 0.700 | 0.785 | 30/30 |
| heldout | or | body | 1/90 | 0.700 | 0.777 | 30/30 |
| dev | and | title | 99/100 | 0.010 | 0.010 | 0/40 |
| dev | or | title | 2/100 | 0.890 | 0.912 | 38/40 |
| dev | or | body | 2/100 | 0.750 | 0.812 | 37/40 |

`and_then_or` (AND rows first, OR rows to fill) equals `or` here, since the AND
lists are empty.

## End to end (`run.py`, `graft retrieve --top-k 5`)

| Set | Kind | n | recall@1 AND → OR | recall@5 AND → OR | MRR AND → OR |
| --- | ---- | -: | -: | -: | -: |
| heldout | paraphrase | 50 | 0.820 → 0.760 | 0.980 → 0.920 | 0.883 → 0.812 |
| heldout | crosslang | 25 | 0.960 → 0.880 | 1.000 → 1.000 | 0.980 → 0.933 |
| heldout | project | 15 | 0.733 → **0.933** | 0.933 → 1.000 | 0.822 → **0.947** |
| heldout | **all answerable** | 90 | **0.844 → 0.822** | 0.978 → 0.956 | **0.900 → 0.868** |
| dev | paraphrase | 50 | 0.960 → 0.940 | 1.000 → 0.980 | 0.977 → 0.955 |
| dev | crosslang | 50 | 1.000 → 0.960 | 1.000 → 0.980 | 1.000 → 0.970 |
| dev | **all answerable** | 100 | **0.980 → 0.950** | 1.000 → 0.980 | **0.988 → 0.963** |

Per query, OR ranks the expected note better on 12 held-out questions and worse
on 14 (dev: 2 better, 5 worse). Retrieve latency p50 281 → 313 ms on held-out,
unchanged on dev.

Raw rows: `and-heldout.json`, `or-heldout.json`, `and-dev.json`, `or-dev.json`
(the `run.py` report format; `rows[].retrieved` is the top 5 per query).

## Why OR loses

The losses are generic-word matches: *"df says 100% used, I deleted the big
app.log but space was not freed"* ranks `pg-serial-sequence-reset` first, an
Italian question about Redis ranks `powershell-execution-policy` first. RRF
gives each list one equal vote and two of the three lists are lexical, so two
noisy BM25 lists outvote a correct vector list. The gains are on project
questions, where exact local terms (`lockfile-guard`, `make dev`, `orbit`)
carry the answer.

Dropping English/Italian stopwords and 1–2 character tokens before OR-ing was
checked on the lexical branch alone and barely moved it (held-out title
recall@1 0.700 → 0.756, body unchanged), so it does not address the fusion.

## Follow-up

The lever is the fusion, not the MATCH expression: weight the lexical lists
below the vector list in RRF (or merge title and body into one lexical vote),
then OR becomes worth re-measuring, with the project-question gain as the
target. Any weight must be chosen on the dev set and reported on held-out.

## Reproducing

`fts_lexical.py` needs no daemon. The end-to-end numbers came from `run.py`
against builds with the join switched by a temporary, uncommitted environment
switch (`MG_FTS_MODE=and|or|and_then_or` read in `storage_fts_search_unlocked`):

```bash
MG_FTS_MODE=or python bench/run.py --graft build/graft.exe --set heldout --out /tmp/or
```
