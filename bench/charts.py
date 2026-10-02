#!/usr/bin/env python3
"""Renders the README charts from the latest benchmark results.

Reads the newest `*-agent.json` and `*-heldout.json` in bench/results and
writes assets/bench-hero.svg (time and tokens, for everyone),
assets/bench-agent.svg (the detail per kind) and assets/bench-recall.svg. Standard library
only; the SVGs are self-contained so GitHub can render them as images.

Usage:
  python bench/charts.py [--results DIR] [--out DIR]
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from xml.sax.saxutils import escape

HERE = Path(__file__).resolve().parent

BG = "#0D1117"
CARD = "#161B22"
BORDER = "#30363D"
TEXT = "#E6EDF3"
MUTED = "#8B949E"
GREEN = "#4CBB6A"
GREY = "#6E7681"
RED = "#F85149"
FONT = "Segoe UI, Helvetica, Arial, sans-serif"


def newest(results: Path, suffix: str) -> dict:
    files = sorted(results.glob(f"*-{suffix}.json"))
    if not files:
        raise SystemExit(f"no *-{suffix}.json in {results}; run the benchmark first")
    return json.loads(files[-1].read_text(encoding="utf-8"))


def text(x, y, s, size=14, fill=TEXT, weight="normal", anchor="start") -> str:
    return (f'<text x="{x}" y="{y}" font-family="{FONT}" font-size="{size}" '
            f'font-weight="{weight}" fill="{fill}" text-anchor="{anchor}">{escape(s)}</text>')


def svg(width: int, height: int, body: list[str]) -> str:
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
            f'viewBox="0 0 {width} {height}">'
            f'<rect width="{width}" height="{height}" rx="16" fill="{BG}"/>'
            + "".join(body) + "</svg>\n")


def panel(x: int, y: int, w: int, title: str, rows: list[tuple[str, float, float, str, str]],
          better: str, scale: float) -> list[str]:
    """One metric: per group, a grey 'without' bar and a green 'with' bar."""
    out = [f'<rect x="{x}" y="{y}" width="{w}" height="250" rx="12" fill="{CARD}" '
           f'stroke="{BORDER}"/>',
           text(x + 20, y + 34, title, 16, weight="600")]
    bar_w = w - 40
    cy = y + 62
    for label, without, with_, s_without, s_with in rows:
        out.append(text(x + 20, cy, label, 13, MUTED))
        for value, shown, color, who in ((without, s_without, GREY, "without"),
                                         (with_, s_with, GREEN, "with graft")):
            cy += 10
            length = max(4.0, bar_w * value / scale) if scale else 4.0
            out.append(f'<rect x="{x + 20}" y="{cy}" width="{length:.1f}" height="22" rx="5" '
                       f'fill="{color}"/>')
            inside = length > 150
            out.append(text(x + 20 + (length - 10 if inside else length + 8), cy + 16,
                            f"{shown}  {who}", 13, BG if inside else TEXT,
                            "600", "end" if inside else "start"))
            cy += 22
        cy += 30
    out.append(text(x + 20, y + 236, better, 12, MUTED))
    return out


def change(a: float, b: float) -> str:
    if not a:
        return ""
    d = (b - a) / a
    return f"{d:+.0%}"


def agent_chart(report: dict) -> str:
    s = report["summary"]
    groups = [("project knowledge", s["project"]), ("general gotchas", s["general"])]
    correct = [(f"{name} · {g['n']} questions", g["without"]["correct_rate"],
                g["with"]["correct_rate"], f"{g['without']['correct_rate']:.0%}",
                f"{g['with']['correct_rate']:.0%}") for name, g in groups]
    time_ = [(name, g["without"]["duration_ms_p50"] / 1000, g["with"]["duration_ms_p50"] / 1000,
              f"{g['without']['duration_ms_p50'] / 1000:.1f} s",
              f"{g['with']['duration_ms_p50'] / 1000:.1f} s") for name, g in groups]
    tokens = [(name, g["without"]["output_tokens_mean"], g["with"]["output_tokens_mean"],
               f"{g['without']['output_tokens_mean']:.0f}",
               f"{g['with']['output_tokens_mean']:.0f}") for name, g in groups]
    max_t = max(max(a, b) for _, a, b, _, _ in time_)
    max_k = max(max(a, b) for _, a, b, _, _ in tokens)
    body = [text(28, 44, "Same questions, with and without graft", 22, weight="700"),
            text(28, 70, f"answers by {report['answer_model']} in a clean Claude Code session, "
                         f"graded by {report['judge_model']} against the team's note · "
                         f"time includes the graft lookup", 13, MUTED)]
    body += panel(28, 92, 356, "Correct answers", correct, "higher is better", 1.0)
    body += panel(400, 92, 356, "Time to answer (median)", time_, "lower is better", max_t)
    body += panel(772, 92, 356, "Output tokens (mean)", tokens, "lower is better", max_k)
    return svg(1156, 370, body)


def median(xs: list[float]) -> float:
    s = sorted(xs)
    n = len(s)
    return (s[n // 2] if n % 2 else (s[n // 2 - 1] + s[n // 2]) / 2) if n else 0.0


def big_card(x: int, y: int, title: str, without: float, with_: float, unit: str,
             fmt: str) -> list[str]:
    """A card a non-technical reader gets at a glance: two bars, one big number."""
    w, h = 536, 250
    drop = (with_ - without) / without if without else 0.0
    out = [f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="14" fill="{CARD}" '
           f'stroke="{BORDER}"/>',
           text(x + 28, y + 44, title, 20, TEXT, "600"),
           text(x + w - 28, y + 52, f"{drop:+.0%}".replace("-", "−"), 46,
                GREEN if drop < 0 else RED, "800", "end")]
    scale = max(without, with_) or 1.0
    bar_max = w - 56
    for i, (label, value, color) in enumerate((("without graft", without, GREY),
                                               ("with graft", with_, GREEN))):
        by = y + 96 + i * 72
        out.append(text(x + 28, by, label, 15, MUTED))
        length = max(6.0, bar_max * value / scale)
        out.append(f'<rect x="{x + 28}" y="{by + 10}" width="{length:.1f}" height="34" rx="8" '
                   f'fill="{color}"/>')
        out.append(text(x + 28 + length - 14, by + 34, f"{fmt.format(value)} {unit}", 18,
                        BG, "700", "end"))
    return out


def hero_chart(report: dict) -> str:
    """Time and tokens per answer, with and without graft, on project questions.

    Project knowledge is what graft is for: a model cannot know a team's own
    conventions. General gotchas, which the model already knows, get one line.
    """
    rows = [r for r in report["rows"] if r["kind"] == "project"]
    t0 = median([r["without"]["duration_ms"] for r in rows]) / 1000
    t1 = median([r["with"]["duration_ms"] for r in rows]) / 1000
    k0 = median([r["without"]["output_tokens"] for r in rows])
    k1 = median([r["with"]["output_tokens"] for r in rows])

    def rate(sel, cond, verdict):
        return sum(r[cond].get("verdict") == verdict for r in sel) / len(sel)

    general = [r for r in report["rows"] if r["kind"] in ("paraphrase", "crosslang")]
    g_tokens = (median([r["with"]["output_tokens"] for r in general])
                / median([r["without"]["output_tokens"] for r in general]) - 1)
    body = [text(28, 48, "Claude Code, with and without graft", 26, TEXT, "700"),
            text(28, 78, f"{len(rows)} questions about a team's own codebase, each asked "
                         "twice. Typical answer (median).", 15, MUTED)]
    body += big_card(28, 100, "Time to answer", t0, t1, "s", "{:.1f}")
    body += big_card(592, 100, "Tokens written", k0, k1, "", "{:.0f}")
    body.append(text(28, 392, f"Right answers: {rate(rows, 'without', 'correct'):.0%} → "
                              f"{rate(rows, 'with', 'correct'):.0%}.   Made-up answers: "
                              f"{rate(rows, 'without', 'invented'):.0%} → "
                              f"{rate(rows, 'with', 'invented'):.0%}.", 16, TEXT, "600"))
    g_t = (median([r["with"]["duration_ms"] for r in general])
           / median([r["without"]["duration_ms"] for r in general]) - 1)
    body.append(text(28, 418, (f"On {len(general)} general coding questions the model already "
                               f"knows: right answers {rate(general, 'without', 'correct'):.0%}"
                               f" → {rate(general, 'with', 'correct'):.0%}, time {g_t:+.0%}, "
                               f"tokens {g_tokens:+.0%}.").replace("-", "−"), 14, MUTED))
    body.append(text(1128, 418, f"model: {report['answer_model']} · graft lookup time included",
                     13, MUTED, anchor="end"))
    return svg(1156, 446, body)


def recall_chart(report: dict) -> str:
    s = report["summary"]
    pos = s["all_positive"]
    neg = s["negative"]["query"]
    lat = report["latency_ms"]["query"]
    bars = [
        ("finds the right note, with confidence", pos["query"]["strong_hit_rate"], GREEN),
        ("confident answers that are right", s["strong_precision"], GREEN),
        ("confident answers to unanswerable questions", neg["false_strong_rate"], RED),
    ]
    body = [text(28, 44, "Recall on questions it was never tuned on", 22, weight="700"),
            text(28, 70, f"{report['corpus']['nodes']} notes · {pos['n']} answerable and "
                         f"{s['negative']['n']} unanswerable questions written independently · "
                         "defaults", 13, MUTED)]
    y = 110
    for label, value, color in bars:
        body.append(text(28, y, label, 14, MUTED))
        length = max(4.0, 760 * value)
        body.append(f'<rect x="28" y="{y + 10}" width="760" height="24" rx="6" fill="{CARD}"/>')
        body.append(f'<rect x="28" y="{y + 10}" width="{length:.1f}" height="24" rx="6" '
                    f'fill="{color}"/>')
        body.append(text(800, y + 28, f"{value:.0%}", 16, TEXT, "700"))
        y += 70
    body.append(f'<rect x="900" y="96" width="228" height="190" rx="12" fill="{CARD}" '
                f'stroke="{BORDER}"/>')
    body.append(text(1014, 140, "lookup time", 14, MUTED, anchor="middle"))
    body.append(text(1014, 200, f"{lat['p50']:.0f} ms", 40, GREEN, "700", "middle"))
    body.append(text(1014, 236, f"median · p95 {lat['p95']:.0f} ms", 13, MUTED, anchor="middle"))
    body.append(text(1014, 260, report["platform"], 12, MUTED, anchor="middle"))
    return svg(1156, 320, body)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--results", default=str(HERE / "results"))
    ap.add_argument("--out", default=str(HERE.parent / "assets"))
    args = ap.parse_args()
    results, out = Path(args.results), Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    agent = newest(results, "agent")
    (out / "bench-hero.svg").write_text(hero_chart(agent), encoding="utf-8")
    (out / "bench-agent.svg").write_text(agent_chart(agent), encoding="utf-8")
    (out / "bench-recall.svg").write_text(recall_chart(newest(results, "heldout")),
                                          encoding="utf-8")
    print(f"wrote bench-hero.svg, bench-agent.svg and bench-recall.svg to {out}")


if __name__ == "__main__":
    main()
