"""MCP server that exposes memgraph operations to chat-based LLMs.

Wraps the `memgraph` CLI via subprocess and parses its JSON-ish output.
The CLI prints valid-JSON-like output (strings quoted, primitives bare),
so we json.loads() the response and return either result or error.

Run:
    python server.py
or via uvx-style installation, see pyproject.toml.

Environment:
    MEMGRAPH_BIN     path to memgraph CLI (default: looks on PATH then ./build/memgraph)
    MEMGRAPH_SOCKET  daemon socket (default /tmp/memgraph.sock)
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
from pathlib import Path
from typing import Any

try:
    from mcp.server.fastmcp import FastMCP
except ImportError as e:
    raise SystemExit(
        "mcp SDK not installed. Run: pip install mcp\n"
        f"original error: {e}"
    )


def _resolve_binary() -> str:
    """Locate the memgraph CLI binary."""
    if env := os.environ.get("MEMGRAPH_BIN"):
        return env
    if found := shutil.which("memgraph"):
        return found
    # Fallback: assume we live next to the build directory.
    candidate = Path(__file__).resolve().parents[2] / "build" / "memgraph"
    if os.name == "nt":
        candidate = candidate.with_suffix(".exe")
    if candidate.exists():
        return str(candidate)
    raise RuntimeError(
        "memgraph CLI not found; set MEMGRAPH_BIN or add it to PATH."
    )


BIN = _resolve_binary()


def _run(args: list[str]) -> dict[str, Any]:
    """Invoke the memgraph CLI and parse its JSON-ish stdout.

    Raises on non-zero exit code (transport failure). Returns the full
    response envelope `{status, result, error?}` as a dict so the caller
    can decide how to render successes vs. server-side errors.
    """
    proc = subprocess.run(
        [BIN, *args],
        capture_output=True,
        text=True,
        encoding="utf-8",
        timeout=60,
    )
    if proc.returncode not in (0, 3):  # 3 = remote handler reported status != 0
        raise RuntimeError(
            f"memgraph CLI failed (rc={proc.returncode}): {proc.stderr.strip()}"
        )
    try:
        return json.loads(proc.stdout)
    except json.JSONDecodeError as e:
        raise RuntimeError(
            f"memgraph CLI emitted unparseable output: {e}\n--- stdout ---\n"
            f"{proc.stdout[:2000]}"
        )


# ---------------------------------------------------------------------------

mcp = FastMCP(
    "memgraph",
    instructions=(
        "Persistent graph memory for AI agents. Use BEFORE solving non-trivial "
        "problems (memgraph_query / memgraph_retrieve / memgraph_explore) "
        "and AFTER novel solutions (memgraph_classify + memgraph_insert)."
    ),
)


@mcp.tool()
def memgraph_query(text: str, signals_only: bool = False) -> dict:
    """Cache lookup with multi-signal gating.

    Returns the best matching node with hit=STRONG|WEAK|MISS. On MISS,
    `fallback_retrieve` carries top-k related candidates.

    Use this BEFORE solving a problem to check if it's already in memory.
    """
    args = ["query", text]
    if signals_only:
        args.extend(["--signals-only"])  # CLI flag may be unsupported; daemon honors arg
    return _run(args)


@mcp.tool()
def memgraph_retrieve(text: str, top_k: int = 10) -> dict:
    """Top-k hybrid retrieval (lexical BM25 + semantic via RRF).

    Returns up to `top_k` nodes ranked by Reciprocal Rank Fusion of
    vector cosine + FTS5 over summary + FTS5 over detail.
    """
    return _run(["retrieve", text, "--top-k", str(top_k)])


@mcp.tool()
def memgraph_explore(
    text: str,
    keywords: list[str] | None = None,
    depth: int = 3,
    beam: int = 4,
) -> dict:
    """Beam search over the memory graph, conditioned on keywords.

    Use this when the user asks "what do you know about X" — explores
    related nodes via keyword + semantic edges with MMR diversity.
    """
    args = ["explore", text, "--depth", str(depth), "--beam", str(beam)]
    for kw in keywords or []:
        args.extend(["--keyword", kw])
    return _run(args)


@mcp.tool()
def memgraph_insert(
    summary: str, detail: str, keywords: list[str] | None = None
) -> dict:
    """Save a new node. Idempotent on (summary+detail+sorted keywords).

    `summary`: title-style 1 line — what future-you searches for.
    `detail`:  the actual answer / fix / pattern, including the WHY.
    `keywords`: 2-5 lowercase tags. Use memgraph_classify to suggest.
    """
    args = ["insert", "--summary", summary, "--detail", detail]
    for kw in keywords or []:
        args.extend(["--keyword", kw])
    return _run(args)


@mcp.tool()
def memgraph_classify(summary: str) -> dict:
    """Suggest up to 15 existing keywords semantically related to `summary`.

    Run this BEFORE memgraph_insert when you don't already have keywords
    in mind, to keep the keyword vocabulary consistent.
    """
    return _run(["classify", "--summary", summary])


@mcp.tool()
def memgraph_get(id_hex: str) -> dict:
    """Fetch a node by its 32-char hex id (returned by other operations)."""
    return _run(["get", id_hex])


@mcp.tool()
def memgraph_stats() -> dict:
    """Inspect runtime metrics: counts (TODO MVP) + similarity distributions."""
    return _run(["stats"])


def main() -> None:
    mcp.run()


if __name__ == "__main__":
    main()
