"""MCP server that exposes graft operations to chat-based LLMs.

The stdio entrypoint is intentionally local/dev friendly:

    python server.py

Production remote MCP is assembled by oauth_gateway.py, which reuses the same
tool registration but runs FastMCP over streamable-http with OAuth resource
server authentication.
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
    from mcp.server.fastmcp.exceptions import ToolError
    from mcp.server.auth.middleware.auth_context import get_access_token
except ImportError as e:
    raise SystemExit(
        "mcp SDK not installed. Run: pip install mcp\n"
        f"original error: {e}"
    )


READ_SCOPE = "graft:read"
WRITE_SCOPE = "graft:write"
ADMIN_SCOPE = "graft:admin"


def _resolve_binary() -> str:
    """Locate the graft CLI binary."""
    if env := os.environ.get("GRAFT_BIN"):
        return env
    if found := shutil.which("graft"):
        return found
    home = Path.home()
    candidates = [
        home / ".graft" / "bin" / ("graft.exe" if os.name == "nt" else "graft"),
        Path(__file__).resolve().parents[2] / "build" / ("graft.exe" if os.name == "nt" else "graft"),
    ]
    for c in candidates:
        if c.exists():
            return str(c)
    raise RuntimeError(
        "graft CLI not found; set GRAFT_BIN, add it to PATH, or run "
        "scripts/install.sh / install.ps1."
    )


_BIN: str | None = None


def _get_binary() -> str:
    global _BIN
    if _BIN is None:
        _BIN = _resolve_binary()
    return _BIN


def _run(args: list[str], profile: Optional[str] = None) -> dict[str, Any]:
    """Invoke the graft CLI and parse its JSON-ish stdout."""
    env = os.environ.copy()
    if profile:
        env["GRAFT_PROFILE"] = profile
    proc = subprocess.run(
        [_get_binary(), *args],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=120,
        env=env,
    )
    if proc.returncode not in (0, 3):  # 3 = handler reported status != 0
        raise RuntimeError(
            f"graft CLI failed (rc={proc.returncode}): {proc.stderr.strip()}"
        )
    out = proc.stdout.strip()
    if not out:
        return {"raw_stdout": "", "raw_stderr": proc.stderr.strip()}
    try:
        return json.loads(out)
    except json.JSONDecodeError:
        return {"raw_stdout": out, "raw_stderr": proc.stderr.strip()}


def _require_scopes(*required: str) -> None:
    """Enforce per-tool scopes for authenticated HTTP MCP requests.

    Stdio calls have no auth context and remain unrestricted for local/dev use.
    The gateway's FastMCP auth middleware already rejects unauthenticated HTTP
    requests before a tool can run.
    """
    token = get_access_token()
    if token is None:
        return
    missing = [scope for scope in required if scope not in token.scopes]
    if missing:
        raise ToolError(f"insufficient OAuth scope; required: {', '.join(missing)}")


def create_mcp(**kwargs: Any) -> FastMCP:
    """Create a FastMCP app and register all graft tools on it."""
    mcp = FastMCP(
        "graft",
        instructions=(
            "Persistent graph memory for AI agents. ALWAYS check before solving "
            "non-trivial problems (graft_query / graft_retrieve / graft_explore) "
            "and save AFTER novel solutions (graft_classify + graft_insert). "
            "Multi-tenant via profiles: pass `profile=<name>` to any tool to target "
            "a specific tenant. Use graft_delete to remove obsolete/wrong nodes; "
            "to 'modify' a node, delete it then re-insert with the corrected fields."
        ),
        **kwargs,
    )
    register_tools(mcp)
    return mcp


def register_tools(mcp: FastMCP) -> FastMCP:
    """Register graft tools on an existing FastMCP instance."""

    @mcp.tool()
    def graft_query(text: str, profile: Optional[str] = None) -> dict:
        """Cache lookup with multi-signal gating."""
        _require_scopes(READ_SCOPE)
        return _run(["query", text], profile=profile)

    @mcp.tool()
    def graft_retrieve(
        text: str, top_k: int = 10, profile: Optional[str] = None
    ) -> dict:
        """Top-k hybrid retrieval (lexical BM25 + semantic via RRF)."""
        _require_scopes(READ_SCOPE)
        return _run(["retrieve", text, "--top-k", str(top_k)], profile=profile)

    @mcp.tool()
    def graft_explore(
        text: str,
        keywords: Optional[list[str]] = None,
        depth: int = 3,
        beam: int = 4,
        profile: Optional[str] = None,
    ) -> dict:
        """Beam search over the memory graph, conditioned on keywords."""
        _require_scopes(READ_SCOPE)
        args = ["explore", text, "--depth", str(depth), "--beam", str(beam)]
        for kw in keywords or []:
            args.extend(["--keyword", kw])
        return _run(args, profile=profile)

    @mcp.tool()
    def graft_classify(title: str, profile: Optional[str] = None) -> dict:
        """Suggest keywords semantically related to `title`."""
        _require_scopes(READ_SCOPE)
        return _run(["classify", "--title", title], profile=profile)

    @mcp.tool()
    def graft_insert(
        title: str,
        body: str,
        keywords: Optional[list[str]] = None,
        author: Optional[str] = None,
        expires_at: Optional[int] = None,
        profile: Optional[str] = None,
    ) -> dict:
        """Save a new node. Idempotent on (title+body+sorted keywords)."""
        _require_scopes(WRITE_SCOPE)
        args = ["insert", "--title", title, "--body", body]
        for kw in keywords or []:
            args.extend(["--keyword", kw])
        if author:
            args.extend(["--author", author])
        if expires_at:
            args.extend(["--expires-at", str(expires_at)])
        return _run(args, profile=profile)

    @mcp.tool()
    def graft_get(id_hex: str, profile: Optional[str] = None) -> dict:
        """Fetch a node by its 32-char hex id."""
        _require_scopes(READ_SCOPE)
        return _run(["get", id_hex], profile=profile)

    @mcp.tool()
    def graft_delete(id_hex: str, profile: Optional[str] = None) -> dict:
        """Remove a node from the graph by its 32-char hex id."""
        _require_scopes(ADMIN_SCOPE)
        return _run(["delete", id_hex], profile=profile)

    @mcp.tool()
    def graft_stats(profile: Optional[str] = None) -> dict:
        """Inspect runtime metrics and similarity distributions."""
        _require_scopes(READ_SCOPE)
        return _run(["stats"], profile=profile)

    @mcp.tool()
    def graft_analytics(
        since: Optional[str] = None,
        seconds_per_hit: int = 60,
        profile: Optional[str] = None,
    ) -> dict:
        """Usage report: hit rate, hoarding ratio, top reused nodes, time saved."""
        _require_scopes(READ_SCOPE)
        args = ["analytics", "--seconds-per-hit", str(seconds_per_hit)]
        if since:
            args.extend(["--since", since])
        return _run(args, profile=profile)

    @mcp.tool()
    def graft_profile_list() -> dict:
        """List all profiles and the active one."""
        _require_scopes(ADMIN_SCOPE)
        return _run(["profile", "list"])

    @mcp.tool()
    def graft_profile_current() -> dict:
        """Show the currently active profile."""
        _require_scopes(ADMIN_SCOPE)
        return _run(["profile", "current"])

    @mcp.tool()
    def graft_profile_add(name: str) -> dict:
        """Create a new profile by name."""
        _require_scopes(ADMIN_SCOPE)
        return _run(["profile", "add", name])

    @mcp.tool()
    def graft_profile_remove(name: str) -> dict:
        """Permanently delete a profile and all its data."""
        _require_scopes(ADMIN_SCOPE)
        return _run(["profile", "remove", name, "--yes"])

    @mcp.tool()
    def graft_profile_export(name: str, path: str) -> dict:
        """Backup a profile to a SQLite file at `path`."""
        _require_scopes(ADMIN_SCOPE)
        return _run(["profile", "export", name, "--path", path])

    @mcp.tool()
    def graft_profile_import(name: str, file: str, force: bool = False) -> dict:
        """Restore a .graftprofile file as a profile named `name`."""
        _require_scopes(ADMIN_SCOPE)
        args = ["profile", "import", "--name", name, "--file", file]
        if force:
            args.append("--force")
        return _run(args)

    @mcp.tool()
    def graft_profile_merge(into_: str, from_: str, overwrite: bool = False) -> dict:
        """Merge nodes/keywords/edges from a .graftprofile file into a profile."""
        _require_scopes(ADMIN_SCOPE)
        args = ["profile", "merge", "--into", into_, "--from", from_]
        if overwrite:
            args.append("--overwrite")
        return _run(args)

    return mcp


mcp = create_mcp()


def main() -> None:
    mcp.run()


if __name__ == "__main__":
    main()
