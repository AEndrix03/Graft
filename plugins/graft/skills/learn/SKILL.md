---
name: learn
description: >-
  Batch knowledge ingestion from external sources (a folder, a codebase, a docs tree). Plans well-shaped nodes with reconciled keywords, shows the plan for approval, then executes idempotently. Triggered by `/learn`, "ingest this folder into memory", "porta questo codice nella memoria", "memorize this codebase". Docs mode (`/learn docs [path]`): all of a repository's documentation and nothing else, incremental on re-runs; "ingest the docs of this repo", "impara la documentazione del repo". Bootstrap mode (`/learn bootstrap [path]`): unattended, progressive cold start of a whole project's memory (topic map, bounded runs by importance, task area first, resumed via `graft project status`); "bootstrap graft for this project", "this project was never bootstrapped", "inizializza la memoria del progetto". Differs from `/memoryze` (1-5 nodes from conversation): `/learn` produces 10-200 nodes from external corpora.
---

# learn — Batch ingestion of external knowledge into graft

`/learn` is a **plan-first** ingestion pipeline. It is NOT "scan and dump everything you see". It is: scout, distill, propose a plan, get the user's approval, then execute. This separation matters because uncontrolled ingestion creates duplicates, garbage summaries, and a keyword vocabulary that fragments into noise. The plan-and-confirm gate is the heart of the skill.

The typical use case: the user points you at a codebase or a docs tree and asks you to "acquire" it — so future-you (or another agent) can `/recall` it without re-reading the source.

## Argument shape

The user invokes you with a free-form prompt that may include:

| Hint                              | Example                                                                | Meaning                                                                  |
| --------------------------------- | ---------------------------------------------------------------------- | ------------------------------------------------------------------------ |
| **target**                        | "this folder", "src/auth/", "the README + docs/", a glob, a list of paths | What to read.                                                            |
| **lens / focus**                  | "as a Spring Boot reference", "extract only public APIs", "design decisions only" | The angle of distillation. Without this you'd save trivia.       |
| **node cap**                      | "max 30 nodes", "atomico per file", "one node per module"              | Soft upper bound; default 50 per invocation, hard cap 200.               |
| **target profile**                | "in profile=docs-springboot", "nel profilo work"                       | `GRAFT_PROFILE=X` for the inserts.                                    |
| **excludes**                      | "skip tests/", "ignore generated/", a `.gitignore`-style list          | Glob/regex skip rules layered on top of the defaults below.              |
| **rerun mode**                    | "incremental" / "force" / "dry-run"                                    | See "Rerunning on the same source" below.                                |
| **docs mode**                     | `docs`, `docs ../other-repo`, `docs from <file>`                       | A repository's documentation only, no plan gate. See "Docs mode" below.  |
| **bootstrap mode**                | `bootstrap`, `bootstrap ../other-repo`, `bootstrap focus src/auth/`   | Unattended, progressive cold start of a whole project. See "Bootstrap mode" below. |

Examples:

- `/learn this Spring Boot codebase, focus on architectural decisions and public REST endpoints, max 40 nodes, profile=app-foo`
- `/learn ingest the docs/ folder as a reference manual, one node per page`
- `/learn questa cartella src/, lente: business rules e gotcha. ignora test/`
- `/learn dry-run on docs/runbooks to see the plan first`

If the first word is `docs`, or the user asks for "the docs / the documentation of this repo", jump to **Docs mode** below: it replaces the six phases. If it is `bootstrap`, or you were told to bootstrap a project that `graft project status` reports as not bootstrapped, jump to **Bootstrap mode**.

If hints are absent, ask **at most one** clarifying question (almost always: "what's the lens?"). Don't ask many small questions; pick reasonable defaults and surface them in the plan for the user to override.

## Pipeline — six phases

```
  Phase 1   discover      list candidate files, apply skip rules
  Phase 2   sample        skim contents to learn topics + structure
  Phase 3   plan          propose N nodes with title + keywords
  Phase 4   confirm       show plan, wait for approval
  Phase 5   ingest        execute: classify + insert per approved node
  Phase 6   report        summarize: created / deduplicated / skipped
```

### Phase 1 — Discover

Walk the target and produce a candidate file list. Apply skip rules in this order:

1. **Hard skips (always)**:
   - `.git/`, `node_modules/`, `target/`, `build/`, `dist/`, `vendor/`, `__pycache__/`
   - Lock files: `package-lock.json`, `yarn.lock`, `poetry.lock`, `Cargo.lock`, `go.sum`
   - Binaries: any file > 1 MB, any non-text file by extension (`.png`, `.jpg`, `.gguf`, `.dll`, `.so`, `.exe`, `.zip`, `.pdf` unless the user said "ingest the PDFs")
   - Generated: `*.min.js`, `*.bundle.*`, files with `do not edit`/`generated by` in the first 3 lines.

2. **User-supplied excludes** (from the prompt).

3. **Project's `.gitignore`** if present (advisory — log when honored).

Output of this phase, kept internally: a numbered list `[(path, size, type), …]`. Surface only the count + a few examples to the user, not the full list.

### Phase 2 — Sample

You don't have time to deeply read every file. Strategy:

- **Always-deep** files: `README*`, top-level `docs/index*`, `ARCHITECTURE*`, `CHANGELOG*`, `*.adr.md`, `pom.xml`, `package.json`, `Cargo.toml`, top-level Makefiles, `*.openapi.{yaml,json}`. These are high-leverage.
- **Sampled** files: for each remaining group (by directory + file extension), read 2-3 representative files in full. Skim the rest with file head (~30 lines).
- **Topic clustering**: group sampled files by directory (or detected feature/module) and infer a topic per group.

Goal of this phase: a topic map like:

```
src/auth/         → "authentication module: JWT issue/verify, bcrypt hashing, refresh flow"
src/payment/      → "payment use cases: Stripe webhook handling, idempotency keys"
docs/runbooks/    → "operational procedures: incident response, deployment, rollback"
README.md         → "project overview + setup quickstart"
```

### Phase 3 — Plan

This is where craftsmanship pays off. For each topic, propose **how many nodes** make sense and what each one should be.

**Node-count heuristic per topic**:

- 1 node when the topic is a single coherent concept (e.g. "JWT issue/verify flow with refresh").
- 2-5 nodes when the topic decomposes naturally into orthogonal facts (rules vs. data model vs. error handling).
- 1 node when the topic is "high-level overview" (READMEs collapse well).
- 0 nodes when the topic is "boilerplate" or "auto-generated config" — say so explicitly.

**Per-node draft**:

```
{
  path_origins: ["src/auth/JwtService.java", "src/auth/RefreshTokenStore.java"],
  title: "<the line future-you will type into search>",
  body_outline: ["why bcrypt cost=12", "refresh token TTL=7d, rotation on use", "trap: clock skew tolerance"],
  keyword_candidates: ["auth", "jwt", "spring-boot", "security"],
}
```

Don't write the full `body` text yet — it's expensive and may be wasted if the user trims the plan. Outline only.

**Keyword reconciliation** (critical for quality):

Before showing the plan, run **one** `graft classify` call per top-K topic to learn what keywords already exist in the graph for similar content:

```bash
# Run per topic, collect suggestions:
graft classify --title "<draft title for this topic>"
```

Aggregate the suggestions across topics and produce a **vocabulary table**:

```
existing in graph: spring-boot, validation, jpa, security, auth, gotcha
proposing new:     refresh-token, bcrypt, openapi, runbook
duplicates folded: spring → spring-boot, validate → validation
```

Use this to align all node `keyword_candidates` to the same vocabulary. The goal is that future searches don't fragment.

**Pre-dedup search** (cheap optimization):

For each draft node, run a quick `graft query` and drop from the plan any candidate the graph already states:

```bash
graft query "<draft title>"
# if the hit's title and body state the same fact: mark as 'already-covered',
# show id_hex in the plan, don't re-insert
```

**The hit level is not the verdict.** Between notes of the same project, and in a young graph, `query` returns STRONG for related but different facts; skipping on STRONG alone silently drops new knowledge. Read the hit and skip only when it says the same thing.

WEAK hits stay in the plan but flagged so the user sees they're refining an existing node.

### Phase 4 — Confirm (REQUIRED gate)

Render the plan as a single readable table the user can scan in 30 seconds:

```
/learn plan — profile=<name>, lens="<focus>", target=<paths>

Discovered: 247 files across 18 dirs, 39 deep-read, 198 sampled, 10 skipped (binaries, lock files)
Proposed nodes: 23  (cap: 40)
Already-covered (skip): 4
WEAK refines existing: 2
Vocabulary: 18 existing keywords reused, 5 new

  #  topic                        nodes  title preview
  ── ─────────────────────────── ───── ───────────────────────────────────────────────────
  1  authentication / JWT         3     "Spring Boot JWT issue + verify + refresh flow"
                                        "bcrypt cost=12 trade-off (CPU vs. brute-force)"
                                        "refresh-token rotation on use prevents replay"
  2  payment / Stripe webhooks    2     "Stripe webhook idempotency key from event.id"
                                        ...
  ...

  ! 2 WEAK refines (will replace existing — review first):
      #14 → existing 019dfceed8db ("sqlite-vec submodule ships only the .h.tmpl…")

  ! 4 already-covered (skip):
      #18 → 019dfcd102 (already covers Spring DI fundamentals)

What now? Reply with one of:
  approve              ingest all 23 nodes
  approve except 14    same, but skip the WEAK refines
  reduce to 10         I'll re-cluster to ~10 highest-impact nodes
  edit                 I'll show each node for inline edit
  cancel               stop, no inserts
  dry-run              simulate ingest, don't write
```

**Rules at this gate**:

- Do NOT insert a single node before the user replies. (Docs and bootstrap modes are the exceptions: their scope and budget are fixed, so the invocation is the approval.)
- If the plan exceeds the user's stated cap (or the default 50), highlight that prominently and propose a reduced version.
- If the plan exceeds the hard cap (200), refuse and ask the user to narrow scope or run `/learn` per subdirectory.

### Phase 5 — Ingest

Once approved, execute the plan in batches. For each approved node:

1. Compose the final `body` text from the outline + sources. **Now** is when you write it, not before.
2. `graft classify --title "<final title>"` for last-mile keyword check.
3. `graft insert --title S --body D --keyword K1 --keyword K2 …`
4. If `result.duplicate=true`, count it but don't error.
5. A new node's result carries `similar` (existing notes close to it). Don't stop on each one: note the pairs and adjudicate them after the batch — the new node corrects the old → `graft maintain resolve --node <old> --action supersede --by <new>`; a conflict the sources cannot settle → `graft maintain resolve --node <new> --action contradicts --by <old>`; only related → nothing. Notes of the plan itself showing up in each other's `similar` are usually just related.

**Profile targeting**: prefix each insert with `GRAFT_PROFILE=<name>` if the user specified a target profile.

**Throughput**: insert ~5-10 nodes/second on a warm daemon. For 50 nodes, ~10s. Don't parallelize — serial is fine and keeps the daemon happy.

**Failure handling**: a single insert failing is not fatal for the batch. Log the error and continue. Report failures in Phase 6.

### Phase 6 — Report

Tight, scannable summary:

```
/learn complete — profile=<name>

Inserted:           19 new nodes
Refined (WEAK→):     2 nodes (existing IDs updated by content-hash dedup)
Skipped (covered):   4
Failed:              0

Vocabulary impact:
  + 5 new keywords: refresh-token, bcrypt, openapi, runbook, idempotency
    (existing graph: 47 keywords → now 52)

Top new nodes:
  ✓ 019dfd... — "Spring Boot JWT issue + verify + refresh flow"
  ✓ 019dfd... — "Stripe webhook idempotency key from event.id"
  …

Try:
  graft query "how do we issue JWTs"
  graft explore "auth" --keyword jwt --keyword security
```

If insertions failed, list them with the error verbatim — surface the real reason. List the conflicts found through `similar` (superseded, or recorded with `contradicts`), so the user can say which side is right.

## Docs mode — `/learn docs [path]`

Ingest **all the documentation of one repository**, and nothing else. `path` defaults to the current repo; a subfolder limits discovery to it, while provenance paths stay relative to the repo root. On the Claude Code plugin it is `/graft:learn docs`.

Scope is fixed: files committed to the repo that are written as documents. **Never** GitHub issues, PRs, review comments, wikis, linked web pages or other repos — not even when a doc links to them. Source code, configs and API specs (OpenAPI, protobuf) are plain `/learn` territory.

The invocation is the approval: there is **no plan gate and no per-node prompt**. In exchange the run is bounded, deterministic and resumable. Every other rule of this page still holds (node shape, keyword reconciliation, pre-dedup, failure handling).

```
  D1  discover     deterministic file list + skip reasons
  D2  diff         with provenance: keep only new / changed docs
  D3  distill      per doc: 0-5 knowledge nodes, dedup against the graph
  D4  insert       with --source file:<path> when available
  D5  report       recap + the exact command that continues
```

### D1 — Discover

Resolve the root (`git -C <path> rev-parse --show-toplevel`) and list **tracked files only** — that honors `.gitignore`, leaves out untracked scratch, and lists each submodule as a single gitlink entry, so no file inside a submodule ever matches:

```bash
cd "$ROOT"
git ls-files | grep -Ei '\.(md|mdx|markdown|rst|adoc|asciidoc|txt)$|(^|/)(readme|contributing|architecture|security|changelog|design|hacking)[^/]*$' | sort
```

Outside a git repo, walk the tree yourself, honor any `.gitignore`, and apply the same rules. Then drop, recording one reason per file:

| Skip | Rule |
| ---- | ---- |
| third-party / generated trees | any path segment `node_modules`, `vendor`, `third_party`, `third-party`, `extern`, `external`, `deps`, `dist`, `build`, `out`, `target`, `_build`, `site-packages`, `.venv`, `venv`, `.git`; anything under a submodule |
| legal boilerplate | `LICENSE*`, `COPYING*`, `NOTICE*`, `CODE_OF_CONDUCT*`, `AUTHORS*` |
| `.txt` that is not prose | any `.txt` outside a docs folder (`docs/`, `doc/`, `documentation/`, `adr/`, `decisions/`, `rfcs/`) except `README.txt`; always `CMakeLists.txt`, `requirements*.txt`, `robots.txt` |
| agent instructions | `CLAUDE.md`, `AGENTS.md`, `GEMINI.md`, `SKILL.md`, `.claude/**`, `.codex/**`, `.cursor/**`, `.github/copilot-instructions.md` — already in the agent's context every session |
| templates and fixtures | `.github/ISSUE_TEMPLATE/**`, `PULL_REQUEST_TEMPLATE*`, paths under `testdata/`, `fixtures/`, `__snapshots__/` |
| generated reports | paths under `results/`, `reports/`, `coverage/` — benchmark or test output, not documentation |
| size / generated | larger than 1 MB, or `generated` / `do not edit` in the first 3 lines |
| user excludes | anything the user named in the prompt |

Lockfiles, binaries and images never match the include pattern. Order the survivors: root `README*` first, then `ARCHITECTURE*`, `docs/**` (index/README of each folder before its pages), ADR folders, the other docs, and `CHANGELOG*` last. The order is what makes batches resumable.

**CHANGELOG rule.** A changelog is history, not documentation. Read it last and take only entries that carry a decision or its rationale, a breaking change, a migration step or a behavior a user must know ("X now does Y because Z"). Skip version bumps, bare fix lists and anything the current docs already state. Never one node per release; at most ~10 nodes from a changelog.

Show the discovery summary before reading anything:

```
/learn docs — root=<repo>, profile=<name>, provenance=<on|off>
Found 41 doc files (186 KB). Skipped 13: 7 agent instructions, 4 submodule trees, 1 license, 1 CMakeLists.txt.
This run: files 1-25 (~150 KB). Remaining after it: 16.
```

### D2 — Diff (provenance)

Provenance makes re-runs incremental. Check it once per run:

```bash
graft sources diff --root "$ROOT"      # JSON with status 0 → provenance on
```

If the command prints usage or a non-zero status, the installed graft predates provenance: **fall back** — insert without `--source`, end each body with a `Source: <repo-relative path>` line instead, and on a re-run rely on the pre-dedup query alone (slower, every doc is re-read). Say which mode is in use in the summary.

With provenance on, `diff` returns `{project, root, summary{sources, unchanged, changed, removed, unavailable, nodes_to_revalidate}, sources[{kind, locator, state, fingerprint, nodes[{id_hex, title, state, recorded_fingerprint}]}]}`; `--changed-only` drops the unchanged entries. `locator` is the repo-relative path, so match it against the discovered list:

- **unchanged** — skip the file.
- **new** (discovered, not recorded) — process normally.
- **changed** — re-read the doc and `graft get` each node it supports. A node that still holds → `graft sources refresh <id>`. A node whose facts changed → insert the corrected node, re-attaching **every** source the old node had, not only this file, then `graft maintain resolve --node <id> --action supersede --by <new-id>`. Knowledge the doc gained → new nodes.
- **unavailable** — the file could not be read (permissions, a broken link): skip it and name it in the report.
- **removed** — do not retire on your own; the knowledge may still be true or the file may have moved (a moved doc re-inserts as `duplicate: true` and gains its new source). List the affected node ids in the report and offer to `retire` them (knowledge gone) or mark them `stale` (still plausible, unverified) with `graft maintain resolve`.

### D3 — Distill

Read each document in full and ask: *what here would a future session search for?* Typical yield:

| Document | Nodes |
| -------- | ----- |
| README | 1-4: what the project is and its central design choice, setup gotchas, conventions |
| guide / reference page | 1 per non-obvious rule, default, limit, trap or procedure |
| ADR / design note | 1 per decision: the choice, the alternatives rejected, why |
| CONTRIBUTING | 1-3: the workflow rules a contributor would otherwise break |
| index, TOC, landing page, badges, generated report, link list | 0 — record "no reusable knowledge" |

Not one summary per file and not a copy of the text: a node holds a fact, a rule or a decision and its why. Keep the house rules — retrieval-shaped title (~80-120 chars, the question future-you types), body under ~1500 chars with the rule, the reason and the trap, a short snippet only when it is the point. Name the project in the title when the fact is specific to it (`graft: ...`), so it does not answer questions about other projects with confidence. A command, flag or default is worth a node only when it is non-obvious or easy to get wrong; reference tables the agent can re-read in the repo are not.

Keywords: 2-4 per node, reconciled with one `graft classify` per document; include the project name as one of them so `explore --keyword <project>` walks the whole repo's knowledge.

Before inserting, run `graft query "<title>"` (and `retrieve --top-k 5` on WEAK). Same knowledge already present → skip, count it. Present but contradicted by the doc, and about this repo → insert the corrected node and supersede the old one with `graft maintain resolve --node <old> --action supersede --by <new>` (the doc is the repo's current truth). A near note on a different subject → insert anyway.

### D4 — Insert and batch

```bash
graft insert --title "..." --body "..." --keyword <project> --keyword k2 \
             --source file:docs/insert/README.md [--source file:README.md]
```

Pass every document the node was distilled from; graft fingerprints the files and stores the paths relative to the repo root. An identical node from a second doc comes back `duplicate: true` with the new source attached — that is correct, not an error. Serial inserts, `GRAFT_PROFILE=<name>` prefix when a profile was named. Collect each result's `similar` and settle it after the batch, as in Phase 5 step 5; name the recorded conflicts in the report.

Bounded batches: per run at most **25 documents or ~300 KB read, and 50 nodes** (hard cap 200, as above). Stop at a document boundary.

### D5 — Report

One paragraph, then the continuation:

```
/learn docs — 25 of 41 files ingested (provenance on): 38 nodes created, 4 updated, 3 refreshed,
9 skipped as already in the graph, 6 files had no reusable knowledge, 0 failures. 2 nodes lose
their source (docs/old-setup.md removed): 019e0a44..., 019e0a51... — retire or keep?
Remaining: 16 files, from docs/storage/README.md.
Continue with: /learn docs from docs/storage/README.md
```

`from <path>` resumes the ordered list at that file. With provenance on, a plain re-run also works: everything ingested is `unchanged` and skipped (only zero-node files are re-skimmed, cheaply).

## Bootstrap mode — `/learn bootstrap [path] [focus <area>]`

The cold start of a project's memory, with nobody watching: no plan gate, no node cap to ask for, no prompt. It covers the whole project (docs, manifests, configuration, public contracts, the implementation), but **progressively**: each run is bounded, spends its budget on the most important uncovered topics, records what is left, and the next run (the next session, the next task in the repo) picks up from there. On the Claude Code plugin it is `/graft:learn bootstrap`.

Only you can judge what is worth remembering, so the bootstrap is your work; graft only keeps the state between runs: `graft project` (the topic map and the runs) and provenance (which files back which nodes).

The goal is **not** a mirror of the repository. It is the knowledge a future session would otherwise have to rediscover by reading many files or by being told: why things are the way they are, what must not be broken, where things live, how to build and run it, what bites. A vague note is worse than no note: it scores high on every question about the project and gets trusted when it does not answer it.

```
  B0  state       graft project status + sources diff -> first run or incremental
  B1  discover    tracked files, skip rules, never secrets, source classes
  B2  topic map   topics with importance and paths, recorded before any insert
  B3  budget      allocate this run's budget by importance (task area first)
  B4  distill     per topic: read, distill, reconcile, dedup, insert with --source
  B5  close       mark covered / pending / skipped, mark the run, report
```

### B0 — State

```bash
ROOT=$(git -C "${path:-.}" rev-parse --show-toplevel)
graft project status --root "$ROOT"    # never starts the daemon, cheap
graft sources diff   --root "$ROOT"    # provenance: which files back which nodes
```

`project status` returns `{project, root, profile, state_file, bootstrapped, runs, last_run_at, provenance: {files, nodes} | null, provenance_error?, topics: {covered, pending, skipped}, pending: [{topic, priority, note, updated_at}], covered: [names], skipped: [names]}`; `pending` is sorted high → normal → low.

- **First run** (`bootstrapped: false`, no topics): do B1-B5 in full.
- **Incremental run** (`bootstrapped: true`): no rescan from scratch. Work, in this order: (1) the task area if any (see B3), (2) `changed` / `removed` sources from `sources diff`, handled exactly like docs-mode D2 (revalidate → `sources refresh`, or insert the correction with every old source and supersede the old node; never retire on `removed`, report it), (3) `pending` topics by priority. Re-run B1 only to spot what is new: a tracked top-level directory, manifest or doc that no topic's note mentions becomes a new pending topic.
- `project` unknown (usage error): the installed graft predates bootstrap state. Run anyway, keep the topic map in your report only, rely on pre-dedup on re-runs. `provenance: null` with a `provenance_error`, or `sources diff` failing: no provenance, so do as docs mode does (a `Source:` line in the body instead of `--source`).

### B1 — Discover

`git ls-files` from the root (tracked only, submodules are single entries), then apply the docs-mode skip rules (third-party and generated trees, legal boilerplate, agent instructions, templates and fixtures, generated reports, size) and add:

| Skip | Rule |
| ---- | ---- |
| **secrets — never open** | `.env*` (except `*.example` / `*.sample` / `*.template`, and then only variable *names*), `*.pem`, `*.key`, `*.p12`, `*.pfx`, `*.jks`, `*.keystore`, `id_rsa*`, `id_ed25519*`, `*.tfvars`, `*.tfstate`, `.npmrc`, `.pypirc`, `.netrc`, `.git-credentials`, `kubeconfig*`, `credentials*`, `secrets*`, `*secret*.y*ml` |
| lockfiles, binaries, media | `*.lock`, `package-lock.json`, `go.sum`, archives, images, fonts, models, anything non-text |
| generated code | protobuf/OpenAPI/ORM output, `*.min.*`, `*.pb.*`, `*_generated.*`, migrations snapshots, files saying `generated` / `do not edit` in their first lines |
| bulk data | fixtures, datasets, `*.csv` / `*.jsonl` corpora, snapshots, test data |
| examples per language | `examples/` trees that repeat one usage in N languages: read one, skip the rest |

**Secrets are never stored**, whatever the file: no credentials, tokens, API keys, passwords, private URLs with credentials, connection strings, cookies, or `.env` values in a title or body. If a file you read holds one, the knowledge you keep is at most "X is configured through the env var `NAME`, set in <where>"; the value never leaves the file.

Sort the survivors into source classes, in this reading order:

1. **overview** — root `README*`, `ARCHITECTURE*`, the docs index, `CONTRIBUTING*` (docs-mode rules).
2. **manifests** — `package.json`, `pyproject.toml`, `Cargo.toml`, `go.mod`, `pom.xml`, `build.gradle*`, `CMakeLists.txt`, `Makefile`, `Dockerfile`, `docker-compose*`, CI workflows: what is built, how, with which toolchain, which checks gate a merge.
3. **configuration** — config schemas and their defaults, example configs, deployment manifests.
4. **contracts** — OpenAPI / protobuf / GraphQL / SQL schema, public headers, exported API modules, CLI argument parsers, wire formats, event payloads.
5. **implementation** — per module (top-level source directories, packages, crates): its entry point and the 1-3 files that carry its core logic. Tests only to confirm an invariant you suspect.
6. **history** (optional, local only) — `git log --no-merges --format='%h %s' -n 300` to spot decisions, reverts and recurring fixes ("why did we switch to X"); read a commit's message body only when its title says a decision. Never fetch issues, PRs or anything remote.

A file larger than ~100 KB is never read whole: read its head and navigate by symbols (`grep -n` for types, entry points, the functions a doc names).

### B2 — Topic map (before any insert)

From the discovery and a skim (heads, directory names, the docs), build the topic map: one topic per module responsibility or cross-cutting concern, not per file.

```
topic (stable name)          importance  kind        paths
overview                     high        overview    README.md, docs/architecture/README.md
build-and-toolchain          high        procedure   CMakeLists.txt, scripts/build-*.sh, .github/workflows/ci.yml
storage                      high        module      src/storage/, include/graft/storage.h, docs/storage/
cli-json-contract            high        contract    src/cli/main.c, docs/cli/README.md
http-viewer                  low         module      src/http/, viewer/
vendored-deps                -           skipped     third_party/ (submodules, not ours)
```

Importance: **high** for the overview, how to build / run / test, the modules on the main data path, public contracts and anything a mistake in would break users; **normal** for secondary modules, integrations, tooling; **low** for peripheral or rarely touched areas (examples, benchmarks, bindings). Areas the user's current task touches are **high** whatever they would be otherwise.

Record the map at once, so an interrupted run still leaves it behind (names are the keys, keep them stable across runs):

```bash
graft project mark --root "$ROOT" --topic storage --state pending --priority high \
                   --note "src/storage/, include/graft/storage.h, docs/storage/"
graft project mark --root "$ROOT" --topic vendored-deps --state skipped --note "third_party/: submodules"
```

`--state` is `covered`, `pending`, `skipped` or `drop` (removes a topic); `--priority` and `--note` are kept when omitted. `--topic` is repeatable, with the same state, priority and note for all of them. Put the paths a topic covers in its note: the next run reads them from `project status` instead of rediscovering them.

### B3 — Budget

Per run, at most **~300 KB read (~40 files) and 40 nodes** (never more than 50). Spend it top-down:

1. **Task relevance first.** Invoked while the user works on something (or with `focus <area>`): the topics that area belongs to come first, even on an incremental run; a touched area no topic covers becomes a new high topic. When bootstrapping alongside a user's task, do not block it: run the bootstrap in a sub-agent / background task if you can, otherwise answer the user first and bootstrap after.
2. Then `high` topics, then `normal`, then `low`, in map order.
3. Rough allotment per topic: overview 2-4 nodes, a module 1-4, a contract 1-3, each procedure 1, each decision 1. A topic that would need more is two topics.

Stop at a topic boundary when the budget is spent. Everything left stays `pending`; that is the point, not a failure.

### B4 — Distill and insert, topic by topic

Read the topic's files and ask, for every candidate: *would a future session search for this, and is it costly to rediscover from the code?* Keep only what passes.

| Keep (knowledge nodes) | Never (they are noise in the graph) |
| ---------------------- | ----------------------------------- |
| architectural decisions and their why, with the rejected alternative | one summary per file, "module X contains files A, B, C" |
| invariants and business rules the code relies on | facts obvious from a signature or a glance at the file |
| a module's responsibility and its boundary (what it must not do) | generated code, boilerplate, config defaults copied verbatim |
| non-obvious constraints: platform quirks, ordering, concurrency, limits | code dumps or long snippets (a snippet only when it *is* the point) |
| setup / build / run / release procedures with their traps | secrets of any kind (see B1) |
| recurring gotchas (from code comments, CONTRIBUTING, history) | near-duplicates of a node already in the graph |
| public contracts: wire / JSON / API shapes and their guarantees | changelog trivia, version bumps |

House rules: retrieval-shaped titles naming the project (`graft: ...`), bodies under ~1500 chars with the rule, the why and the trap, 2-4 keywords including the project name. Then, per topic:

1. `graft classify --title "<a representative title>"` once, reconcile the topic's keywords with the existing vocabulary (as in Phase 3).
2. Per node, `graft query "<title>"` (and `retrieve --top-k 5` on WEAK): already known → skip and count; contradicted by the current code, and about this project → insert the corrected node, then `graft maintain resolve --node <old> --action supersede --by <new>`; different subject → insert. **A hit level is not a verdict: read the hit's title and body.** In a young graph, and between notes of the same project, `query` returns STRONG for unrelated facts (in a test, every draft after the first came back STRONG on the first node); skipping on the level alone would stop the bootstrap after one node. Skip only when the hit states the same fact.
3. Insert with every file the node was distilled from: `graft insert --title ... --body ... --keyword <project> ... --source file:<path> [--source file:<path2>]`. Run it from the root, or pass absolute paths. `duplicate: true` is fine. Settle the results' `similar` after the topic, as in Phase 5 step 5.
4. `graft project mark --topic <t> --state covered` when done; `--state skipped --note "<why>"` when the topic held nothing reusable (say so, do not force nodes).

### B5 — Close and report

```bash
graft project mark --root "$ROOT" --run    # bootstrapped: true, runs + 1, last_run_at
```

Mark the run even when topics remain: it means "a bootstrap has happened", not "everything is covered". Then end with the machine-readable report for the agent (or the orchestrator) that invoked you, followed by one line for the human:

```json
{"bootstrap": {
  "project": "github.com/owner/repo", "mode": "first|incremental", "provenance": true,
  "topics": {"covered": ["overview", "storage"], "pending": ["http-viewer"], "skipped": ["vendored-deps"]},
  "files_read": 31, "kb_read": 240,
  "nodes": {"created": 27, "replaced": 1, "refreshed": 3, "duplicate": 2, "skipped_known": 6, "failed": 0},
  "sources": {"changed": 2, "removed": 1, "nodes_orphaned": ["019e0a44..."]},
  "next": "/learn bootstrap"
}}
```

```
graft bootstrap: 27 notes on 9 topics of repo (3 pending, next run: http-viewer, bindings, bench).
```

A re-run with nothing changed and nothing pending reads no file and creates no node: `sources diff` says everything is `unchanged`, `project status` has no pending topic, and the report says so in one line.

## Rerunning on the same source

Three modes:

- **incremental (default)**: re-run Phase 1-3, but in Phase 3 the pre-dedup `query` step naturally suppresses already-saved knowledge. Useful when the source has grown since the last `/learn`.
- **force**: skip the pre-dedup `query` step. Useful when summaries need to be re-saved with better phrasing because the user has new lens; will produce duplicates flagged by content-hash if truly identical.
- **dry-run**: complete Phase 1-4, render the plan, **never** ingest. Useful to preview before committing to the run.

## Optimization rules of thumb

- **Read budget** ~300 KB total per `/learn` invocation. If the corpus is bigger, sample harder.
- **Classify budget**: at most 1 call per topic in Phase 3 (not per file). Aggregate suggestions across topics.
- **Pre-dedup budget**: 1 `query` per draft node — cheap; do it always. Worth it.
- **Body composition**: only after approval (Phase 5), and never write more than ~1500 chars per body. Future-you skims; doesn't read essays.

## What this skill does NOT do

- It does **not** modify the source files. Read-only on the codebase.
- It does **not** auto-promote nodes to docs/README. That's a `/memory-audit` follow-up suggestion.
- It does **not** re-classify existing graph nodes (no `update` API yet).
- It does **not** auto-run on file changes. Pair with a watcher externally if you want that workflow.

## Failure modes

| Symptom                                              | Cause / Action                                                          |
| ---------------------------------------------------- | ----------------------------------------------------------------------- |
| Plan has 0 nodes                                     | Lens too narrow, or corpus is mostly skip-rule matches. Loosen lens.    |
| Plan has > hard cap (200)                            | Scope too wide. Run `/learn` per subdirectory, or tighten the lens.     |
| Many WEAK refines (> 30% of plan)                    | The graph already covers this corpus. Suggest user run `/memory-audit`. |
| classify suggests no keywords                        | Graph still cold; manually infer keywords from the lens + topic.        |
| Daemon connection fails mid-batch                    | Stop, surface the error. Don't half-ingest silently.                    |
