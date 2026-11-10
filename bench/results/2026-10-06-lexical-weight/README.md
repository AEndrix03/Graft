# Lexical weight in `retrieve` RRF — 2026-10-06

Follow-up to [`2026-10-05-fts-match`](../2026-10-05-fts-match/README.md) for
[#24](https://github.com/AEndrix03/Graft/issues/24): on the base corpus, OR-joined
BM25 lists with a down-weighted RRF vote looked better on held-out but not on
dev, by one or two questions, and the dev set had no project questions at all.
This run adds [`bench/corpus-ext`](../../corpus-ext/) (45 project notes, 57 dev
and 90 held-out project questions, 30 + 30 project negatives) and repeats the
comparison.

**Decision: adopt OR-joined BM25 lists with lexical weight 0.1**, now
`retrieval.lexical_weight` (default `0.1`). The weight was chosen on the
extended dev set; the extended held-out set was used only to report.

graft 0.2.0 (develop), Windows, built-in defaults, BGE-M3,
`run.py --extend bench/corpus-ext` (110 notes). Variants ran from a temporary
build switch (`MG_FTS_MODE`, `MG_RRF_LEX_W`); the committed implementation
reproduces the `or-0.1` rows (see *Check* below).

## Choosing on dev

recall@1 / MRR, `retrieve --top-k 5`:

| Variant | all (157) | project (57) | general (100) |
| ------- | -: | -: | -: |
| AND (old) | 0.841 / 0.883 | 0.596 / 0.697 | 0.980 / 0.988 |
| OR, w = 1 | 0.860 / 0.891 | 0.737 / 0.799 | 0.930 / 0.944 |
| OR, w = 0.25 | 0.854 / 0.895 | 0.684 / 0.771 | 0.950 / 0.966 |
| **OR, w = 0.1** | 0.854 / **0.903** | 0.702 / 0.792 | 0.940 / 0.967 |
| OR, w = 0.05 | 0.860 / 0.903 | 0.719 / 0.788 | 0.940 / 0.968 |

Selection rule: best dev MRR over all answerable questions. w = 0.1 (0.9034)
edges w = 0.05 (0.9028); both are far from w = 1, which buys project recall
with a large loss on general questions.

## Reporting on held-out

| Variant | all (180) | project (105) | general (75) |
| ------- | -: | -: | -: |
| AND (old) | 0.778 / 0.832 | 0.714 / 0.777 | 0.867 / 0.909 |
| OR, w = 1 | 0.744 / 0.806 | 0.743 / 0.810 | 0.747 / 0.801 |
| OR, w = 0.25 | 0.783 / 0.842 | 0.743 / 0.811 | 0.840 / 0.886 |
| **OR, w = 0.1** | **0.789 / 0.848** | **0.743 / 0.807** | 0.853 / 0.906 |
| OR, w = 0.05 | 0.772 / 0.842 | 0.705 / 0.787 | 0.867 / 0.918 |

Paired per question against AND (expected note ranked higher / lower; two-sided
sign test):

| Set | Variant | all | project | general |
| --- | ------- | -: | -: | -: |
| dev | OR, w = 0.1 | 16 / 7 (p = 0.09) | 14 / 2 (p = 0.004) | 2 / 5 |
| held-out | OR, w = 0.1 | 25 / 14 (p = 0.11) | 18 / 7 (p = 0.04) | 7 / 7 |
| held-out | OR, w = 1 | 32 / 27 (p = 0.60) | 24 / 10 (p = 0.02) | 8 / 17 (p = 0.11) |

The gain is where it was expected and significant on both sets: questions about
a team's own code, whose local identifiers (service names, commands, flags,
tables) the embedding model cannot know. General questions are unchanged at
w = 0.1 and clearly hurt at w = 1. `query` (STRONG precision 0.760 dev, 0.660
held-out) is identical for every variant: its gating does not use FTS.

Retrieve latency p50, AND → OR w = 0.1: 266 → 297 ms on dev, 321 → 328 ms on
held-out (the two BM25 lists now return rows to fuse).

## Caveats

- The dev and held-out questions were written by two agents that never saw
  each other's file, but they are the same model on the same notes: 6 came out
  identical and 27 nearly so. The 34 dev questions with word Jaccard > 0.5 to a
  held-out one were removed; the sets still share scenarios. With one
  parameter picked among five values, the room to overfit is small, and the
  held-out gain matches the dev gain in direction.
- On the base corpus alone (no extension), w = 0.1 moved held-out recall@1
  0.844 → 0.867 and dev 0.980 → 0.970
  ([2026-10-05-fts-match](../2026-10-05-fts-match/README.md)): no harm beyond one
  question.
- Rows: `<variant>-<set>.json`, the `run.py` report format.

## Check

The committed build (OR join, `retrieval.lexical_weight: 0.1`, no switches)
was re-run on the same sets. On the extended corpus it reproduces the `or-0.1`
rows exactly (0 rank differences on dev and held-out); on the base corpus it
gives the same numbers as the base-corpus w = 0.1 run of 2026-10-05:

| Corpus | Set | recall@1 / MRR | STRONG precision (`query`) |
| ------ | --- | -: | -: |
| extended | dev | 0.854 / 0.903 | 0.760 |
| extended | held-out | 0.789 / 0.848 | 0.660 |
| base | dev | 0.970 / 0.983 | 0.916 |
| base | held-out | 0.867 / 0.920 | 0.823 |
