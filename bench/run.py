#!/usr/bin/env python3
"""Reproducible quality + latency benchmark for graft.

Starts a private graftd (own GRAFT_HOME, socket, DB and usage log, built-in
default settings), inserts bench/corpus/nodes.jsonl, then runs every query in
bench/corpus/queries.jsonl through `graft query` and `graft retrieve` exactly
as an agent would. Your real graph and usage log are never touched.

Reports, per query kind (exact title, English paraphrase, Italian query on an
English node, unrelated/near-topic negative):

  query     correct STRONG, correct WEAK, wrong STRONG, MISS; STRONG precision
  retrieve  recall@1, recall@k, MRR
  latency   p50 / p95 per operation, measured by the CLI round trip

Usage:
  python bench/run.py [--graft PATH] [--model PATH] [--top-k 5] [--out DIR]

Standard library only.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import platform
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
KINDS = ["exact", "paraphrase", "crosslang", "negative"]


# ---------- graft plumbing ----------

def find_graft(explicit: str | None) -> Path:
    if explicit:
        return Path(explicit).resolve()
    found = shutil.which("graft")
    if not found:
        sys.exit("graft not found on PATH; pass --graft")
    return Path(found)


def find_graftd(graft: Path) -> Path:
    for name in ("graftd.exe", "graftd"):
        p = graft.with_name(name)
        if p.exists():
            return p
    sys.exit(f"graftd not found next to {graft}")


def default_model() -> Path:
    home = os.environ.get("GRAFT_HOME") or str(Path.home() / ".graft")
    return Path(home) / "models" / "bge-m3.gguf"


class Graft:
    """A private daemon plus a CLI wrapper pointed at it."""

    def __init__(self, graft: Path, model: Path, workdir: Path):
        self.graft = graft
        self.workdir = workdir
        self.config = workdir / "config.yaml"
        # Same shape the installers write: only the paths, defaults elsewhere.
        self.config.write_text(
            "daemon:\n"
            f'  socket_path: "{(workdir / "graft.sock").as_posix()}"\n'
            f'  db_path: "{(workdir / "graft.db").as_posix()}"\n'
            "\n"
            "embedding:\n"
            f'  model_path: "{model.as_posix()}"\n',
            encoding="utf-8",
        )
        self.env = {k: v for k, v in os.environ.items() if not k.startswith("GRAFT_")}
        self.env.update(
            GRAFT_HOME=str(workdir),
            GRAFT_CONFIG=str(self.config),
            GRAFT_SOCKET=str(workdir / "graft.sock"),
            GRAFT_DB_PATH=str(workdir / "graft.db"),
            GRAFT_AUTHOR="bench",
        )
        self.daemon: subprocess.Popen | None = None
        self.log_path = workdir / "graftd.log"

    def start(self, timeout_s: float = 120.0) -> None:
        log = open(self.log_path, "wb")
        self.daemon = subprocess.Popen(
            [str(find_graftd(self.graft)), "--config", str(self.config)],
            env=self.env, stdout=log, stderr=subprocess.STDOUT,
        )
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            if self.daemon.poll() is not None:
                sys.exit(f"graftd exited during startup:\n{self.log_tail()}")
            if b"graftd: listening on" in self.log_path.read_bytes():
                return
            time.sleep(0.2)
        self.stop()
        sys.exit(f"graftd not ready after {timeout_s:.0f}s:\n{self.log_tail()}")

    def stop(self) -> None:
        if self.daemon and self.daemon.poll() is None:
            self.daemon.terminate()
            try:
                self.daemon.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.daemon.kill()
                self.daemon.wait()

    def log_tail(self, n: int = 20) -> str:
        try:
            lines = self.log_path.read_text(errors="replace").splitlines()
        except OSError:
            return "(no log)"
        return "\n".join(lines[-n:])

    def run(self, *args: str) -> tuple[dict, float]:
        t0 = time.perf_counter()
        proc = subprocess.run(
            [str(self.graft), *args], env=self.env,
            capture_output=True, text=True, encoding="utf-8",
        )
        wall_ms = (time.perf_counter() - t0) * 1000.0
        try:
            out = json.loads(proc.stdout)
        except json.JSONDecodeError:
            sys.exit(f"graft {' '.join(args)} printed non-JSON (rc={proc.returncode}):\n"
                     f"{proc.stdout}\n{proc.stderr}")
        if out.get("status") != 0:
            sys.exit(f"graft {' '.join(args)} failed: {out}")
        return out["result"], wall_ms

    def usage_latencies(self) -> dict[str, list[int]]:
        """CLI-measured round trips, from this run's private usage log."""
        by_op: dict[str, list[int]] = {}
        path = self.workdir / "usage.jsonl"
        if not path.exists():
            return by_op
        for line in path.read_text(encoding="utf-8").splitlines():
            try:
                ev = json.loads(line)
            except json.JSONDecodeError:
                continue
            by_op.setdefault(ev["op"], []).append(int(ev["latency_ms"]))
        return by_op


# ---------- metrics ----------

def pct(values: list[float], p: float) -> float | None:
    if not values:
        return None
    s = sorted(values)
    k = (len(s) - 1) * p
    lo, hi = int(k), min(int(k) + 1, len(s) - 1)
    return s[lo] + (s[hi] - s[lo]) * (k - lo)


def ratio(n: int, d: int) -> float | None:
    return n / d if d else None


def summarize(rows: list[dict], top_k: int) -> dict:
    summary: dict = {}
    for kind in KINDS + ["all_positive"]:
        sel = [r for r in rows if (r["kind"] == kind if kind != "all_positive"
                                   else r["expect"] is not None)]
        if not sel:
            continue
        n = len(sel)
        c = {o: sum(1 for r in sel if r["outcome"] == o)
             for o in ("correct_strong", "correct_weak", "wrong_strong", "wrong_weak", "miss")}
        entry = {"n": n, "query": c}
        if kind != "negative":
            ranks = [r["rank"] for r in sel]
            entry["query"]["strong_hit_rate"] = ratio(c["correct_strong"], n)
            entry["retrieve"] = {
                "recall@1": ratio(sum(1 for x in ranks if x == 1), n),
                f"recall@{top_k}": ratio(sum(1 for x in ranks if x), n),
                "mrr": sum(1.0 / x for x in ranks if x) / n,
            }
        else:
            entry["query"]["false_strong_rate"] = ratio(c["wrong_strong"], n)
            entry["query"]["correct_miss_rate"] = ratio(c["miss"], n)
        summary[kind] = entry
    strong = [r for r in rows if r["hit"] == "STRONG"]
    summary["strong_precision"] = ratio(
        sum(1 for r in strong if r["outcome"] == "correct_strong"), len(strong))
    summary["weak_count"] = sum(1 for r in rows if r["hit"] == "WEAK")
    return summary


def fmt(x: float | None, digits: int = 3) -> str:
    return "—" if x is None else f"{x:.{digits}f}"


def markdown(report: dict) -> str:
    s, top_k = report["summary"], report["top_k"]
    out = [
        f"# graft benchmark — {report['started_at'][:10]}",
        "",
        f"graft {report['graft_version']} · {report['platform']} · "
        f"{report['corpus']['nodes']} nodes · {report['corpus']['queries']} queries · "
        "built-in default settings",
        "",
        "## Verified cache (`graft query`)",
        "",
        "| Kind | n | correct STRONG | correct WEAK | wrong STRONG | wrong WEAK | MISS |",
        "| ---- | -: | -: | -: | -: | -: | -: |",
    ]
    for kind in KINDS:
        if kind in s:
            q = s[kind]["query"]
            out.append(f"| {kind} | {s[kind]['n']} | {q['correct_strong']} | {q['correct_weak']} "
                       f"| {q['wrong_strong']} | {q['wrong_weak']} | {q['miss']} |")
    neg = s.get("negative", {}).get("query", {})
    out += [
        "",
        f"- **STRONG precision** (a STRONG hit points at the right node): "
        f"{fmt(s['strong_precision'])}",
        f"- **STRONG hit rate on answerable queries**: "
        f"{fmt(s['all_positive']['query']['strong_hit_rate'])}",
        f"- **False STRONG on negatives**: {fmt(neg.get('false_strong_rate'))}",
        f"- **WEAK answers overall**: {s['weak_count']}",
        "",
        "## Hybrid retrieval (`graft retrieve`)",
        "",
        f"| Kind | n | recall@1 | recall@{top_k} | MRR |",
        "| ---- | -: | -: | -: | -: |",
    ]
    for kind in ["exact", "paraphrase", "crosslang", "all_positive"]:
        if kind in s:
            r = s[kind]["retrieve"]
            out.append(f"| {kind} | {s[kind]['n']} | {fmt(r['recall@1'])} "
                       f"| {fmt(r[f'recall@{top_k}'])} | {fmt(r['mrr'])} |")
    out += [
        "",
        "## Latency (ms, CLI round trip to the daemon)",
        "",
        "| Op | n | p50 | p95 | max | p50 incl. process start |",
        "| -- | -: | -: | -: | -: | -: |",
    ]
    for op, lat in report["latency_ms"].items():
        out.append(f"| {op} | {lat['n']} | {fmt(lat['p50'], 0)} | {fmt(lat['p95'], 0)} "
                   f"| {fmt(lat['max'], 0)} | {fmt(lat['wall_p50'], 0)} |")
    return "\n".join(out) + "\n"


# ---------- main ----------

def load_jsonl(path: Path) -> list[dict]:
    return [json.loads(l) for l in path.read_text(encoding="utf-8").splitlines() if l.strip()]


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--graft", help="graft CLI (default: the one on PATH)")
    ap.add_argument("--model", help="BGE-M3 GGUF (default: $GRAFT_HOME/models/bge-m3.gguf)")
    ap.add_argument("--top-k", type=int, default=5)
    ap.add_argument("--corpus", default=str(HERE / "corpus"))
    ap.add_argument("--out", default=str(HERE / "results"))
    ap.add_argument("--keep", action="store_true", help="keep the temporary GRAFT_HOME")
    args = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8")

    graft = find_graft(args.graft)
    model = Path(args.model).resolve() if args.model else default_model()
    if not model.exists():
        sys.exit(f"model not found: {model} (pass --model)")
    nodes = load_jsonl(Path(args.corpus) / "nodes.jsonl")
    queries = load_jsonl(Path(args.corpus) / "queries.jsonl")
    keys = {n["key"] for n in nodes}
    unknown = {q["expect"] for q in queries if q["expect"] and q["expect"] not in keys}
    if unknown:
        sys.exit(f"queries reference unknown node keys: {sorted(unknown)}")
    # Exact-title queries are derived, so they can never drift from the corpus.
    queries = [{"query": n["title"], "expect": n["key"], "kind": "exact"} for n in nodes] + queries

    version = subprocess.run([str(graft), "--version"], capture_output=True,
                             text=True).stdout.strip().removeprefix("graft ")
    started = dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds")
    workdir = Path(tempfile.mkdtemp(prefix="graft-bench-"))
    g = Graft(graft, model, workdir)
    wall: dict[str, list[float]] = {}
    rows: list[dict] = []
    print(f"graft {version}, {len(nodes)} nodes, {len(queries)} queries, home {workdir}",
          file=sys.stderr)
    try:
        g.start()
        key_of: dict[str, str] = {}
        for i, n in enumerate(nodes, 1):
            argv = ["insert", "--title", n["title"], "--body", n["body"]]
            for kw in n.get("keywords", []):
                argv += ["--keyword", kw]
            res, ms = g.run(*argv)
            wall.setdefault("insert", []).append(ms)
            key_of[res["id_hex"]] = n["key"]
            print(f"\rinsert {i}/{len(nodes)}", end="", file=sys.stderr)
        print(file=sys.stderr)

        for i, q in enumerate(queries, 1):
            qres, qms = g.run("query", q["query"])
            rres, rms = g.run("retrieve", q["query"], "--top-k", str(args.top_k))
            wall.setdefault("query", []).append(qms)
            wall.setdefault("retrieve", []).append(rms)
            hit = qres.get("hit", "MISS")
            got = key_of.get(qres.get("id_hex", ""), None) if hit != "MISS" else None
            if hit == "MISS":
                outcome = "miss"
            else:
                right = q["expect"] is not None and got == q["expect"]
                outcome = ("correct_" if right else "wrong_") + hit.lower()
            ranked = [key_of.get(r["id_hex"]) for r in rres.get("results", [])]
            rank = ranked.index(q["expect"]) + 1 if q["expect"] in ranked else 0
            rows.append({**q, "hit": hit, "got": got, "outcome": outcome, "rank": rank,
                         "signals": qres.get("signals"), "retrieved": ranked})
            print(f"\rquery {i}/{len(queries)}", end="", file=sys.stderr)
        print(file=sys.stderr)
    finally:
        g.stop()

    usage = g.usage_latencies()
    latency = {}
    for op in ("insert", "query", "retrieve"):
        vals = usage.get(op, [])
        latency[op] = {"n": len(vals), "p50": pct(vals, 0.5), "p95": pct(vals, 0.95),
                       "max": max(vals) if vals else None,
                       "wall_p50": pct(wall.get(op, []), 0.5)}

    report = {
        "started_at": started,
        "graft_version": version,
        "platform": f"{platform.system()} {platform.machine()}",
        "cpu": platform.processor() or None,
        "cpu_count": os.cpu_count(),
        "settings": "built-in defaults (installer-style config with paths only)",
        "corpus": {"nodes": len(nodes), "queries": len(queries)},
        "top_k": args.top_k,
        "summary": summarize(rows, args.top_k),
        "latency_ms": latency,
        "rows": rows,
    }
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    stem = f"{started[:10]}-{platform.system().lower()}-{version}"
    (out_dir / f"{stem}.json").write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n",
                                         encoding="utf-8")
    md = markdown(report)
    (out_dir / f"{stem}.md").write_text(md, encoding="utf-8")
    print(md)
    print(f"wrote {out_dir / stem}.json and .md", file=sys.stderr)
    if args.keep:
        print(f"kept {workdir}", file=sys.stderr)
    else:
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    main()
