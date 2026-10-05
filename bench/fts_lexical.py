#!/usr/bin/env python3
"""The lexical branch of `graft retrieve`, alone.

`retrieve` fuses three ranked lists with RRF: vector cosine, BM25 over the
title and BM25 over the body. This script rebuilds the two BM25 lists for the
bench corpus in an in-memory SQLite FTS5 table (same tokenizer and per-token
column scoping as src/storage/storage.c) and scores them on their own, for
three ways of joining the query tokens:

  and          `title:"a" title:"b"`      implicit AND, what graft does
  or           `title:"a" OR title:"b"`   any token, ranked by BM25
  and_then_or  the AND rows first, then OR rows to fill the list

Per strategy and column: how many answerable questions get an empty list,
recall@1 / recall@5 / MRR of the expected note within the list, and how many
unanswerable questions still get a non-empty list.

No daemon and no model: it shows what each strategy feeds the fusion, not the
end-to-end result (measure that with run.py).

Usage:
  python bench/fts_lexical.py [--corpus DIR]

Standard library only (needs an SQLite built with FTS5, as CPython's is).
"""

import argparse
import json
import sqlite3
from pathlib import Path

HERE = Path(__file__).resolve().parent
PER_LIST_K = 50  # MG_RETRIEVE_PER_LIST_K in src/retrieve/retrieve.c


def load_jsonl(path: Path) -> list[dict]:
    return [json.loads(l) for l in path.read_text(encoding="utf-8").splitlines() if l.strip()]


def scoped(col: str, query: str, sep: str) -> str:
    return sep.join(f'{col}:"' + tok.replace('"', '""') + '"' for tok in query.split())


def search(db: sqlite3.Connection, col: str, query: str, mode: str) -> list[str]:
    rank = "bm25(f, 1.0, 0.0)" if col == "title" else "bm25(f, 0.0, 1.0)"
    seps = {"and": [" "], "or": [" OR "], "and_then_or": [" ", " OR "]}[mode]
    out: list[str] = []
    for sep in seps:
        rows = db.execute(f"SELECT key FROM f WHERE f MATCH ? ORDER BY {rank} LIMIT ?",
                          (scoped(col, query, sep), PER_LIST_K))
        for (key,) in rows:
            if key not in out and len(out) < PER_LIST_K:
                out.append(key)
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--corpus", default=str(HERE / "corpus"))
    corpus = Path(ap.parse_args().corpus)

    nodes = load_jsonl(corpus / "nodes.jsonl") + load_jsonl(corpus / "project_nodes.jsonl")
    db = sqlite3.connect(":memory:")
    db.execute("CREATE VIRTUAL TABLE f USING fts5(title, body, key UNINDEXED, "
               "tokenize='unicode61 remove_diacritics 2')")
    db.executemany("INSERT INTO f(title, body, key) VALUES (?, ?, ?)",
                   [(n["title"], n["body"], n["key"]) for n in nodes])

    print("| Set | Strategy | List | empty (answerable) | recall@1 | recall@5 | MRR "
          "| non-empty (negatives) |")
    print("| --- | -------- | ---- | -: | -: | -: | -: | -: |")
    for query_set, name in (("heldout", "heldout.jsonl"), ("dev", "queries.jsonl")):
        queries = load_jsonl(corpus / name)
        pos = [q for q in queries if q["expect"]]
        neg = [q for q in queries if not q["expect"]]
        for mode in ("and", "or", "and_then_or"):
            for col in ("title", "body"):
                ranks, empty = [], 0
                for q in pos:
                    got = search(db, col, q["query"], mode)
                    empty += not got
                    ranks.append(got.index(q["expect"]) + 1 if q["expect"] in got else 0)
                neg_hits = sum(1 for q in neg if search(db, col, q["query"], mode))
                n = len(pos)
                print(f"| {query_set} | {mode} | {col} | {empty}/{n} "
                      f"| {sum(r == 1 for r in ranks) / n:.3f} "
                      f"| {sum(0 < r <= 5 for r in ranks) / n:.3f} "
                      f"| {sum(1 / r for r in ranks if r) / n:.3f} "
                      f"| {neg_hits}/{len(neg)} |")


if __name__ == "__main__":
    main()
