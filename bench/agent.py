#!/usr/bin/env python3
"""What graft changes for an agent: the same questions, with and without it.

For every held-out question the same model answers twice, in a clean Claude
Code session (`claude -p --safe-mode`: no CLAUDE.md, hooks, plugins, skills or
MCP servers, no tools):

  without  the question only
  with     the question plus whatever `graft query` returns for it, injected
           the way the optional prompt hook does: the note on STRONG/WEAK,
           nothing on MISS. Wrong hits are injected too - that is the real cost
           of a confident wrong answer. Its time includes the graft lookup
           (process start plus round trip), hit or miss.

A second model then grades each answer against the note that really answers
the question (or, for unanswerable questions, checks whether an injected note
derailed the answer). Reported per kind: share of correct answers, share of
invented answers, output tokens (thinking included), wall time and cost.

Needs the `claude` CLI logged in. Every call is billed to that account.

Usage:
  python bench/agent.py [--answer-model sonnet] [--judge-model haiku] [--workers 4]
"""

from __future__ import annotations

import argparse
import concurrent.futures as cf
import datetime as dt
import json
import platform
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run import (HERE, Graft, default_model, find_graft, graft_version,  # noqa: E402
                 insert_nodes, load_corpus, pct)

SYSTEM = (
    "You are a coding agent working in the Orbit monorepo (TypeScript, Python, "
    "Postgres, Kubernetes, GitHub Actions). A developer on the team asks you a "
    "question. Answer it directly: the likely cause and what to do. Be concise."
)

MEMORY = (
    "\n\n<graft-memory hit=\"{hit}\">\nA note from the team's memory graph that may "
    "be relevant. Check that it applies before relying on it.\n\nTitle: {title}\n\n"
    "{body}\n</graft-memory>"
)

JUDGE_POSITIVE = """You grade an answer given by a coding agent.

Question: {question}

Reference note (the team's verified answer):
Title: {ref_title}
{ref_body}

Agent's answer:
{answer}

Classify the answer:
- "correct": it identifies the same cause and a fix consistent with the reference note.
- "abstained": it does not commit to a cause/fix (says it does not know, asks for details,
  only lists generic possibilities without picking one).
- "invented": it confidently gives a cause or fix that differs from or contradicts the reference.

Reply with JSON only: {{"verdict": "correct" | "abstained" | "invented"}}"""

JUDGE_NEGATIVE = """You grade an answer given by a coding agent.

Question: {question}

The agent may have been shown this note, which does NOT answer the question:
{injected}

Agent's answer:
{answer}

Did the answer wrongly apply that note, i.e. present the note's cause or fix as the
answer to this question? If no note was shown, the answer cannot have been misled.

Reply with JSON only: {{"misled": true | false}}"""


def claude(prompt: str, model: str, system: str | None = None, timeout: int = 300) -> dict:
    cmd = ["claude", "-p", "--safe-mode", "--model", model, "--output-format", "json",
           "--tools", ""]
    if system:
        cmd += ["--system-prompt", system]
    for attempt in range(3):
        proc = subprocess.run(cmd, input=prompt, capture_output=True, text=True,
                              encoding="utf-8", timeout=timeout, cwd=tempfile.gettempdir())
        try:
            out = json.loads(proc.stdout)
        except json.JSONDecodeError:
            out = {"is_error": True, "result": proc.stdout + proc.stderr}
        if not out.get("is_error"):
            return out
    raise RuntimeError(f"claude failed 3 times: {str(out.get('result'))[:300]}")


def parse_verdict(text: str) -> dict:
    start, end = text.find("{"), text.rfind("}")
    try:
        return json.loads(text[start:end + 1])
    except (ValueError, json.JSONDecodeError):
        return {}


def answer(question: str, memory: dict | None, model: str) -> dict:
    prompt = question + (MEMORY.format(**memory) if memory else "")
    out = claude(prompt, model, SYSTEM)
    usage = out.get("usage", {})
    return {
        "answer": out.get("result", ""),
        "output_tokens": usage.get("output_tokens", 0),
        "input_tokens": usage.get("input_tokens", 0) + usage.get("cache_read_input_tokens", 0)
        + usage.get("cache_creation_input_tokens", 0),
        "duration_ms": out.get("duration_ms", 0),
        "cost_usd": out.get("total_cost_usd", 0.0),
    }


def judge(q: dict, ref: dict | None, memory: dict | None, ans: str, model: str) -> dict:
    if ref:
        prompt = JUDGE_POSITIVE.format(question=q["query"], ref_title=ref["title"],
                                       ref_body=ref["body"], answer=ans)
    else:
        injected = f"Title: {memory['title']}\n{memory['body']}" if memory else "(no note shown)"
        prompt = JUDGE_NEGATIVE.format(question=q["query"], injected=injected, answer=ans)
    return parse_verdict(claude(prompt, model).get("result", ""))


def run_one(q: dict, lookup: dict, nodes: dict, args) -> dict:
    ref = nodes.get(q["expect"]) if q["expect"] else None
    memory = lookup if lookup["hit"] != "MISS" else None
    row = {**q, "hit": lookup["hit"], "got": lookup["key"], "lookup_ms": lookup["lookup_ms"]}
    for cond, mem in (("without", None), ("with", memory)):
        a = answer(q["query"], mem, args.answer_model)
        if cond == "with":
            # The agent pays for the lookup too, hit or miss.
            a["duration_ms"] += lookup["lookup_ms"]
        v = judge(q, ref, mem, a["answer"], args.judge_model)
        row[cond] = {**a, **v}
    return row


def mean(xs: list[float]) -> float | None:
    return sum(xs) / len(xs) if xs else None


def summarize(rows: list[dict]) -> dict:
    groups = {
        "general": [r for r in rows if r["kind"] in ("paraphrase", "crosslang")],
        "project": [r for r in rows if r["kind"] == "project"],
        "negative": [r for r in rows if r["kind"] == "negative"],
    }
    summary = {}
    for name, sel in groups.items():
        if not sel:
            continue
        entry = {"n": len(sel)}
        for cond in ("without", "with"):
            c = [r[cond] for r in sel]
            e = {
                "output_tokens_mean": mean([x["output_tokens"] for x in c]),
                "duration_ms_p50": pct([x["duration_ms"] for x in c], 0.5),
                "duration_ms_mean": mean([x["duration_ms"] for x in c]),
                "cost_usd_total": sum(x["cost_usd"] for x in c),
            }
            if name == "negative":
                e["misled_rate"] = mean([1.0 if x.get("misled") else 0.0 for x in c])
            else:
                for verdict in ("correct", "abstained", "invented"):
                    e[f"{verdict}_rate"] = mean(
                        [1.0 if x.get("verdict") == verdict else 0.0 for x in c])
            entry[cond] = e
        summary[name] = entry
    return summary


def markdown(report: dict) -> str:
    s = report["summary"]
    out = [
        f"# graft agent benchmark — {report['started_at'][:10]}",
        "",
        f"graft {report['graft_version']} · answers by `{report['answer_model']}`, "
        f"graded by `{report['judge_model']}` · {report['questions']} questions × 2",
        "",
        "| Questions | n | correct without → with | invented without → with "
        "| output tokens without → with | time p50 (s) without → with |",
        "| --------- | -: | -: | -: | -: | -: |",
    ]
    for name in ("general", "project"):
        if name not in s:
            continue
        a, b = s[name]["without"], s[name]["with"]
        out.append(
            f"| {name} | {s[name]['n']} | {a['correct_rate']:.0%} → {b['correct_rate']:.0%} "
            f"| {a['invented_rate']:.0%} → {b['invented_rate']:.0%} "
            f"| {a['output_tokens_mean']:.0f} → {b['output_tokens_mean']:.0f} "
            f"| {a['duration_ms_p50'] / 1000:.1f} → {b['duration_ms_p50'] / 1000:.1f} |")
    if "negative" in s:
        a, b = s["negative"]["without"], s["negative"]["with"]
        out += ["", f"Unanswerable questions ({s['negative']['n']}): answers misled by a "
                    f"wrongly injected note: {b['misled_rate']:.0%}."]
    total = sum(s[k][c]["cost_usd_total"] for k in s for c in ("without", "with"))
    out += ["", f"Total cost of the answers: ${total:.2f} (list price)."]
    return "\n".join(out) + "\n"


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--graft", help="graft CLI (default: the one on PATH)")
    ap.add_argument("--model", help="BGE-M3 GGUF (default: $GRAFT_HOME/models/bge-m3.gguf)")
    ap.add_argument("--answer-model", default="sonnet")
    ap.add_argument("--judge-model", default="haiku")
    ap.add_argument("--workers", type=int, default=4)
    ap.add_argument("--limit", type=int, help="only the first N questions of each kind")
    ap.add_argument("--corpus", default=str(HERE / "corpus"))
    ap.add_argument("--out", default=str(HERE / "results"))
    args = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8")
    if not shutil.which("claude"):
        sys.exit("the claude CLI is not on PATH")

    graft = find_graft(args.graft)
    model = Path(args.model).resolve() if args.model else default_model()
    nodes, queries = load_corpus(Path(args.corpus), "heldout")
    if args.limit:
        seen: dict[str, int] = {}
        kept = []
        for q in queries:
            seen[q["kind"]] = seen.get(q["kind"], 0) + 1
            if seen[q["kind"]] <= args.limit:
                kept.append(q)
        queries = kept
    by_key = {n["key"]: n for n in nodes}

    version = graft_version(graft)
    started = dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds")
    workdir = Path(tempfile.mkdtemp(prefix="graft-agent-bench-"))
    g = Graft(graft, model, workdir)
    memories: list[dict] = []
    try:
        g.start()
        key_of = insert_nodes(g, nodes, {})
        for q in queries:
            res, ms = g.run("query", q["query"])
            hit = res.get("hit", "MISS")
            memories.append({"hit": hit, "title": res.get("title", ""),
                             "body": res.get("body", ""), "lookup_ms": ms,
                             "key": key_of.get(res.get("id_hex", ""))})
    finally:
        g.stop()
        shutil.rmtree(workdir, ignore_errors=True)

    with cf.ThreadPoolExecutor(max_workers=args.workers) as pool:
        futs = [pool.submit(run_one, q, m, by_key, args) for q, m in zip(queries, memories)]
        for i, _ in enumerate(cf.as_completed(futs), 1):
            print(f"\ranswered {i}/{len(futs)}", end="", file=sys.stderr)
    print(file=sys.stderr)
    rows = [f.result() for f in futs]

    report = {
        "started_at": started,
        "graft_version": version,
        "platform": f"{platform.system()} {platform.machine()}",
        "answer_model": args.answer_model,
        "judge_model": args.judge_model,
        "questions": len(rows),
        "summary": summarize(rows),
        "rows": rows,
    }
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    stem = f"{started[:10]}-{platform.system().lower()}-{version}-agent"
    (out_dir / f"{stem}.json").write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n",
                                         encoding="utf-8")
    md = markdown(report)
    (out_dir / f"{stem}.md").write_text(md, encoding="utf-8")
    print(md)
    print(f"wrote {out_dir / stem}.json and .md", file=sys.stderr)


if __name__ == "__main__":
    main()
