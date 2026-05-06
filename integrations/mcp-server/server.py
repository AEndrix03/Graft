"""MCP server that exposes memgraph operations to chat-based LLMs.

Wraps the `memgraph` CLI via subprocess and parses its JSON-ish output.
The CLI prints valid-JSON-like output (strings quoted, primitives bare),
so we json.loads() the response and return either result or error.

Run:
    python server.py
or via uvx-style installation, see pyproject.toml.

Environment:
    MEMGRAPH_BIN     path to memgraph CLI (default: looks on PATH then ./build/memgraph)
    MEMGRAPH_SOCKET  daemon socket override (per-profile default if unset)
    MEMGRAPH_PROFILE active profile (default "default"). Overridable per call.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
from pathlib import Path
from typing import Any, Optional

try:
    from mcp.server.fastmcp import FastMCP
except ImportError as e:
    raise SystemExit(
        "mcp SDK not installed. Run: pip install mcp\n"
        f"original error: {e}"
    )


def _resolve_binary() -> str:
    """Locate the memgraph CLI binary.

    Resolution order: $MEMGRAPH_BIN → PATH → user install
    (~/.lmemorygraph/bin/memgraph[.exe]) → repo build dir.
    """
    if env := os.environ.get("MEMGRAPH_BIN"):
        return env
    if found := shutil.which("memgraph"):
        return found
    home = Path.home()
    candidates = [
        home / ".lmemorygraph" / "bin" / ("memgraph.exe" if os.name == "nt" else "memgraph"),
        Path(__file__).resolve().parents[2] / "build" / ("memgraph.exe" if os.name == "nt" else "memgraph"),
    ]
    for c in candidates:
        if c.exists():
            return str(c)
    raise RuntimeError(
        "memgraph CLI not found; set MEMGRAPH_BIN, add it to PATH, or run "
        "scripts/install.sh / install.ps1."
    )


BIN = _resolve_binary()


def _run(args: list[str], profile: Optional[str] = None) -> dict[str, Any]:
    """Invoke the memgraph CLI and parse its JSON-ish stdout.

    `profile`, when non-None, is exported as MEMGRAPH_PROFILE for this
    invocation only — letting the caller target a specific profile per
    operation without mutating shell state.

    Raises on non-zero exit code (transport failure). Returns the full
    response envelope `{status, result, error?}` so the caller can
    decide how to render successes vs. server-side errors.
    """
    env = os.environ.copy()
    if profile:
        env["MEMGRAPH_PROFILE"] = profile
    proc = subprocess.run(
        [BIN, *args],
        capture_output=True,
        text=True,
        encoding="utf-8",
        # The daemon's stderr can include locale-encoded bytes on Windows
        # (cp1252) — don't crash the whole call on a stray byte.
        errors="replace",
        timeout=120,
        env=env,
    )
    if proc.returncode not in (0, 3):  # 3 = handler reported status != 0
        raise RuntimeError(
            f"memgraph CLI failed (rc={proc.returncode}): {proc.stderr.strip()}"
        )
    out = proc.stdout.strip()
    if not out:
        # `profile set` (no --global) prints to stdout *and* a tip on stderr
        # but the stdout may legitimately be a non-JSON shell line. Wrap.
        return {"raw_stdout": "", "raw_stderr": proc.stderr.strip()}
    try:
        return json.loads(out)
    except json.JSONDecodeError:
        # Return non-JSON output as raw — happens for `profile set` etc.
        return {"raw_stdout": out, "raw_stderr": proc.stderr.strip()}


# ---------------------------------------------------------------------------

mcp = FastMCP(
    "memgraph",
    instructions=(
        "Persistent graph memory for AI agents. ALWAYS check before solving "
        "non-trivial problems (memgraph_query / memgraph_retrieve / memgraph_explore) "
        "and save AFTER novel solutions (memgraph_classify + memgraph_insert). "
        "Multi-tenant via profiles — pass `profile=<name>` to any tool to target "
        "a specific tenant. Use memgraph_delete to remove obsolete/wrong nodes; "
        "to 'modify' a node, delete it then re-insert with the corrected fields."
    ),
)


# === SEARCH ===

@mcp.tool()
def memgraph_query(text: str, profile: Optional[str] = None) -> dict:
    """Cache lookup with multi-signal gating.

    Returns the best matching node with hit=STRONG|WEAK|MISS. On MISS,
    `fallback_retrieve` carries top-k related candidates.

    Use this BEFORE solving a problem to check if it's already in memory.
    """
    return _run(["query", text], profile=profile)


@mcp.tool()
def memgraph_retrieve(
    text: str, top_k: int = 10, profile: Optional[str] = None
) -> dict:
    """Top-k hybrid retrieval (lexical BM25 + semantic via RRF).

    Use when you want a ranked list rather than a single best hit, e.g.
    when the question is open-ended ("what do we know about X").
    """
    return _run(["retrieve", text, "--top-k", str(top_k)], profile=profile)


@mcp.tool()
def memgraph_explore(
    text: str,
    keywords: Optional[list[str]] = None,
    depth: int = 3,
    beam: int = 4,
    profile: Optional[str] = None,
) -> dict:
    """Beam search over the memory graph, conditioned on keywords.

    Use when the user names a topic + keyword anchors and wants related
    knowledge surfaced via graph walk.
    """
    args = ["explore", text, "--depth", str(depth), "--beam", str(beam)]
    for kw in keywords or []:
        args.extend(["--keyword", kw])
    return _run(args, profile=profile)


# === SAVE ===

@mcp.tool()
def memgraph_classify(summary: str, profile: Optional[str] = None) -> dict:
    """Suggest keywords semantically related to `summary`.

    Run BEFORE memgraph_insert to keep the keyword vocabulary consistent
    across the graph. Returns up to ~15 suggestions; falls back to inferred
    novel keywords on a sparse graph.
    """
    return _run(["classify", "--summary", summary], profile=profile)


@mcp.tool()
def memgraph_insert(
    summary: str,
    detail: str,
    keywords: Optional[list[str]] = None,
    profile: Optional[str] = None,
) -> dict:
    """Save a new node. Idempotent on (summary+detail+sorted keywords).

    `summary`: title-style 1 line — what future-you searches for. ~80-120 chars.
    `detail`:  the actual answer / fix / pattern, including the WHY.
    `keywords`: 3-6 lowercase tags. Use memgraph_classify to suggest first.
    """
    args = ["insert", "--summary", summary, "--detail", detail]
    for kw in keywords or []:
        args.extend(["--keyword", kw])
    return _run(args, profile=profile)


# === GET / DELETE / STATS ===

@mcp.tool()
def memgraph_get(id_hex: str, profile: Optional[str] = None) -> dict:
    """Fetch a node by its 32-char hex id (returned by other operations)."""
    return _run(["get", id_hex], profile=profile)


@mcp.tool()
def memgraph_delete(id_hex: str, profile: Optional[str] = None) -> dict:
    """Remove a node from the graph by its 32-char hex id.

    Cascades: node_keywords, edges, FTS row, and vector embedding are
    cleaned up automatically. Returns NOT_FOUND if the id doesn't exist.

    To 'modify' a node, fetch it (memgraph_get), delete it, then insert a
    new node with the corrected summary/detail/keywords. The insert
    pipeline rebuilds the embedding and edges from scratch.
    """
    return _run(["delete", id_hex], profile=profile)


@mcp.tool()
def memgraph_stats(profile: Optional[str] = None) -> dict:
    """Inspect runtime metrics: node/edge/keyword counts + similarity distributions."""
    return _run(["stats"], profile=profile)


@mcp.tool()
def memgraph_analytics(
    since: Optional[str] = None,
    seconds_per_hit: int = 60,
    profile: Optional[str] = None,
) -> dict:
    """Usage report: hit rate, hoarding ratio, top reused nodes, time saved.

    `since`: optional window like "7d", "24h", "3600s". Default = lifetime.
    `seconds_per_hit`: estimated agent-reasoning time saved per STRONG hit
    (default 60s). The total `estimated_seconds_saved` = STRONG hits * this.
    """
    args = ["analytics", "--seconds-per-hit", str(seconds_per_hit)]
    if since:
        args.extend(["--since", since])
    return _run(args, profile=profile)


# === PROFILES ===

@mcp.tool()
def memgraph_profile_list() -> dict:
    """List all profiles + the active one.

    Each profile is a tenant-isolated graph (separate DB + daemon).
    """
    return _run(["profile", "list"])


@mcp.tool()
def memgraph_profile_current() -> dict:
    """Show the currently active profile (resolves $MEMGRAPH_PROFILE or 'default')."""
    return _run(["profile", "current"])


@mcp.tool()
def memgraph_profile_add(name: str) -> dict:
    """Create a new profile by name. Names must match [a-zA-Z0-9_-]{1,64}."""
    return _run(["profile", "add", name])


@mcp.tool()
def memgraph_profile_remove(name: str) -> dict:
    """Permanently delete a profile and ALL its data. Auto-confirms (--yes).

    Refuses 'default' (not removable) and any profile whose daemon is up.
    """
    return _run(["profile", "remove", name, "--yes"])


@mcp.tool()
def memgraph_profile_export(name: str, path: str) -> dict:
    """Backup a profile to a SQLite file at `path`.

    The exported file is a self-contained .mgprofile (= SQLite DB) that
    can be reopened by `memgraph_profile_import` or merged with
    `memgraph_profile_merge`.
    """
    return _run(["profile", "export", name, "--path", path])


@mcp.tool()
def memgraph_profile_import(name: str, file: str, force: bool = False) -> dict:
    """Restore a .mgprofile file as a profile named `name`.

    `force=True` overwrites if the target profile already exists.
    Refuses if the target profile's daemon is currently running.
    """
    args = ["profile", "import", "--name", name, "--file", file]
    if force:
        args.append("--force")
    return _run(args)


@mcp.tool()
def memgraph_profile_merge(into_: str, from_: str, overwrite: bool = False) -> dict:
    """Merge nodes/keywords/edges from a .mgprofile file into an existing profile.

    Idempotent: nodes deduplicated by content_hash, keywords reconciled by text.
    `overwrite=True` adopts the source's metadata (created_at, access_count)
    on duplicates; default skip keeps the target's existing metadata.
    Refuses if the target's daemon is running (would corrupt the WAL).
    """
    args = ["profile", "merge", "--into", into_, "--from", from_]
    if overwrite:
        args.append("--overwrite")
    return _run(args)


def main() -> None:
    mcp.run()


if __name__ == "__main__":
    main()
