"""End-to-end checks of query / retrieve / explore / `hook prompt` against a
real embedding model.

Starts a private graftd (the `Graft` harness of bench/run.py), inserts a
handful of notes and asserts on what the CLI returns. Every "absent from the
results" assertion has a positive control taken before the note left the
active state, so an empty result cannot pass by accident.

    python tests/e2e/test_e2e.py --graft build/graft

The model is GRAFT_TEST_MODEL (a GGUF path). Without it the suite exits 77,
which ctest reports as skipped.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "bench"))
from run import Graft  # noqa: E402

SKIP = 77

# rank-1 RRF score of a note found by the vector list alone (rrf_k_const 60):
# anything above it means the BM25 lists voted too.
VECTOR_ONLY_TOP = 1.0 / 61.0

WAL = ("SQLite WAL mode needs a shared-memory file next to the database",
       "With journal_mode=WAL SQLite creates the -wal and -shm files beside the "
       "database. On network shares the shared-memory file cannot be mapped, so "
       "WAL fails there; use DELETE journaling on SMB and NFS.")
BUSY = ("SQLite busy_timeout avoids SQLITE_BUSY errors with concurrent writers",
        "Two connections writing the same SQLite database get SQLITE_BUSY unless "
        "busy_timeout is set: sqlite3_busy_timeout(db, 5000) retries the lock.")
GRADLE = ("Gradle configuration cache breaks tasks that read project at execution time",
          "With --configuration-cache, a task action that touches `project` fails. "
          "Capture the values in providers at configuration time instead.")
PROBE = ("Kubernetes liveness probe restarts the pod when the health endpoint hangs",
         "A failing livenessProbe makes the kubelet kill and restart the container; "
         "a slow dependency behind /healthz therefore causes restart loops.")
FETCH_OLD = ("Node.js has no global fetch, install node-fetch",
             "Older Node versions need the node-fetch package for HTTP requests.")
FETCH_NEW = ("Node.js 18 ships a global fetch based on undici",
             "From Node 18 fetch is global and built on undici; node-fetch is not needed.")
TEMP = ("Temporary note: the staging database is read-only during the migration",
        "Writes to the staging database fail while the schema migration runs.")


class Suite:
    def __init__(self, g: Graft):
        self.g = g
        self.failures: list[str] = []

    def cli(self, *args: str) -> dict:
        return self.g.run(*args)[0]

    def insert(self, note: tuple[str, str], *keywords: str, expires_at: int = 0) -> str:
        args = ["insert", "--title", note[0], "--body", note[1]]
        for k in keywords:
            args += ["--keyword", k]
        if expires_at:
            args += ["--expires-at", str(expires_at)]
        return self.cli(*args)["id_hex"]

    def resolve(self, node: str, action: str, *extra: str) -> None:
        self.cli("maintain", "resolve", "--node", node, "--action", action, *extra)

    def hook_prompt(self, prompt: str) -> str:
        proc = subprocess.run(
            [str(self.g.graft), "hook", "prompt"], env=self.g.env,
            input=json.dumps({"prompt": prompt, "cwd": str(self.g.workdir)}),
            capture_output=True, text=True, encoding="utf-8",
        )
        if proc.returncode != 0:
            self.fail(f"hook prompt exited {proc.returncode}: {proc.stderr}")
        return proc.stdout

    def retrieve_ids(self, text: str) -> list[str]:
        return [r["id_hex"] for r in self.cli("retrieve", text)["results"]]

    def explore_ids(self, text: str, keyword: str) -> list[str]:
        out = self.cli("explore", text, "--keyword", keyword)
        return [n["id_hex"] for n in out["nodes"]]

    def check(self, ok: bool, what: str) -> None:
        print(("ok   " if ok else "FAIL ") + what)
        if not ok:
            self.failures.append(what)

    def fail(self, what: str) -> None:
        self.check(False, what)

    def run(self) -> int:
        now_ms = int(time.time() * 1000)
        wal = self.insert(WAL, "sqlite")
        busy = self.insert(BUSY, "sqlite")
        gradle = self.insert(GRADLE, "gradle")
        probe = self.insert(PROBE, "kubernetes")
        old = self.insert(FETCH_OLD, "nodejs")
        new = self.insert(FETCH_NEW, "nodejs")
        temp_expiry = now_ms + 8000
        temp = self.insert(TEMP, "staging", expires_at=temp_expiry)

        # ---- query gating ----
        q = self.cli("query", WAL[0])
        self.check(q.get("hit") == "STRONG" and q.get("id_hex") == wal,
                   "query: verbatim title is a STRONG hit on its note")
        self.check(q.get("state") == "active", "query: an active note reports state active")
        q = self.cli("query", "how long should sourdough bread proof in the fridge")
        self.check(q.get("hit") != "STRONG", "query: an unrelated question is not STRONG")

        # ---- retrieve: ranking and the lexical lists ----
        r = self.cli("retrieve", "why does sqlite wal fail on a network share")["results"]
        self.check(bool(r) and r[0]["id_hex"] == wal,
                   "retrieve: natural-language question ranks its note first")
        self.check(bool(r) and r[0]["score"] > VECTOR_ONLY_TOP,
                   f"retrieve: BM25 lists contribute (top score {r[0]['score'] if r else None}"
                   f" > vector-only {VECTOR_ONLY_TOP:.6f})")

        # ---- explore ----
        ids = self.explore_ids("sqlite locking and journaling", "sqlite")
        self.check(wal in ids and busy in ids, "explore: keyword filter reaches both sqlite notes")

        # ---- positive controls, before the lifecycle changes ----
        self.check(probe in self.retrieve_ids("kubernetes liveness probe restart loop"),
                   "control: the to-be-retired note is retrievable")
        self.check(self.cli("query", PROBE[0]).get("id_hex") == probe,
                   "control: the to-be-retired note is a query hit")
        self.check(old in self.retrieve_ids("node fetch http request"),
                   "control: the to-be-superseded note is retrievable")
        self.check(temp in self.retrieve_ids("staging database read-only migration"),
                   "control: the expiring note is retrievable before it expires")
        hook = self.hook_prompt(GRADLE[0])
        self.check(gradle in hook and "STALE" not in hook,
                   "control: hook prompt injects the active note without a stale warning")

        # ---- lifecycle ----
        self.resolve(gradle, "stale")
        self.resolve(probe, "retire")
        self.resolve(old, "supersede", "--by", new)
        time.sleep(max(0.0, (temp_expiry - time.time() * 1000) / 1000.0) + 1.0)

        # stale: still found, flagged everywhere (#17)
        q = self.cli("query", GRADLE[0])
        self.check(q.get("id_hex") == gradle and q.get("state") == "stale",
                   "stale: query still returns the note, with state stale")
        r = self.cli("retrieve", "gradle configuration cache task project")["results"]
        self.check(any(x["id_hex"] == gradle and x["state"] == "stale" for x in r),
                   "stale: retrieve returns the note with state stale")
        hook = self.hook_prompt(GRADLE[0])
        self.check(gradle in hook and "STALE" in hook, "stale: hook prompt labels the note STALE")

        # retired, superseded, expired: gone
        self.check(probe not in self.retrieve_ids("kubernetes liveness probe restart loop"),
                   "retired: absent from retrieve")
        self.check(probe not in json.dumps(self.cli("query", PROBE[0])),
                   "retired: absent from query (hit and fallback)")
        self.check(old not in self.retrieve_ids("node fetch http request"),
                   "superseded: absent from retrieve")
        self.check(new in self.retrieve_ids("node fetch http request"),
                   "superseded: its replacement is retrievable")
        self.check(temp not in self.retrieve_ids("staging database read-only migration"),
                   "expired: absent from retrieve")
        self.check(temp not in json.dumps(self.cli("query", TEMP[0])),
                   "expired: absent from query")

        print(f"\n{len(self.failures)} failure(s)")
        return 1 if self.failures else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--graft", required=True, help="path to the graft CLI (graftd next to it)")
    args = ap.parse_args()

    model = os.environ.get("GRAFT_TEST_MODEL")
    if not model or not Path(model).is_file():
        print("skip: set GRAFT_TEST_MODEL to a GGUF embedding model")
        return SKIP

    # Short on purpose: an AF_UNIX socket path past ~108 chars makes graftd
    # fail with "socket listen failed" (Windows temp dirs are deep).
    workdir = Path(tempfile.mkdtemp(prefix="ge2e"))
    g = Graft(Path(args.graft).resolve(), Path(model).resolve(), workdir)
    try:
        g.start()
        return Suite(g).run()
    finally:
        g.stop()
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
