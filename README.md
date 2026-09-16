<div align="center">

<img src="./assets/graft.png" alt="Graft logo" width="180"/>

# graft

### Your coding agent already learned this. Graft makes sure it doesn't forget.

**Persistent local memory for AI coding agents.**  
Graft brings back useful fixes, decisions, gotchas and project knowledge when they become relevant again.

The agent still reasons. **Graft gives it a head start.**

<br/>

[![GitHub Stars](https://img.shields.io/github/stars/AEndrix03/Graft?style=flat-square)](https://github.com/AEndrix03/Graft/stargazers)
[![Release](https://img.shields.io/github/v/release/AEndrix03/Graft?style=flat-square)](https://github.com/AEndrix03/Graft/releases)
[![License: Apache 2.0](https://img.shields.io/badge/license-Apache%202.0-blue.svg?style=flat-square)](./LICENSE)
[![Platforms](https://img.shields.io/badge/Linux%20%7C%20macOS%20%7C%20Windows-supported-lightgrey.svg?style=flat-square)](./docs/install/)
[![Local first](https://img.shields.io/badge/local--first-no%20SaaS-success.svg?style=flat-square)](#local-by-default)

<br/>

**Claude Code · Codex · ChatGPT · Claude Desktop · Gemini CLI · Open Code · custom agents**

<sub>C11 · SQLite · sqlite-vec · FTS5 · BGE-M3 · llama.cpp · MCP · MessagePack</sub>

</div>

---

## Graft in 20 seconds

Your agent solves something difficult.

**Graft remembers the useful part.**

Later, another session hits a similar problem.

**Graft surfaces the old learning before the agent wastes time rediscovering it.**

```text
solve something
      │
      ▼
 remember what mattered
      │
      ▼
     Graft
      │
      ├── likely same problem ──► verified recall
      ├── related knowledge ────► hybrid retrieval
      └── broader context ──────► graph exploration
                                  │
                                  ▼
                              your agent
```

**No SaaS. No external embedding API. No account. No API key.**

Graft does not replace the agent's reasoning. It gives the agent relevant prior knowledge and lets the agent decide what to do with it.

---

## Stop solving the same problem twice

Without persistent memory:

```text
session 1
bug → investigate → understand → fix → context disappears

session 27
similar bug → investigate → understand → fix → context disappears
```

With Graft:

```text
session 1
bug → investigate → fix → remember

session 27
similar bug → recall → decide → continue
```

Graft is useful for knowledge that is expensive to rediscover:

- root causes that took hours to find
- architectural decisions and why they were made
- framework and infrastructure gotchas
- project-specific conventions
- dependency constraints
- failed approaches worth avoiding
- fixes that may apply again

This is **agent memory**, not document storage.

---

## Install

**Linux**

```bash
curl -fsSL https://raw.githubusercontent.com/AEndrix03/Graft/master/install.sh | sh
```

**Windows**

```powershell
irm https://raw.githubusercontent.com/AEndrix03/Graft/master/install.ps1 | iex
```

**macOS** — prebuilt archives aren't published yet, so use the tap:

```bash
brew tap AEndrix03/graft https://github.com/AEndrix03/Graft.git && brew install graft
```

The one-liners drop prebuilt, checksum-verified binaries into `~/.graft` — no
compiler, no submodules, no MSYS2. The embedding model (~600 MB) is downloaded
once. Nothing else to configure.

Then wire it into your coding agent, which is two commands:

```bash
graft setup     # copies the skills into every agent found on this machine
/graft-init     # run this inside the agent; it asks one question and writes the rule
```

Prefer Scoop on Windows?

```powershell
scoop install https://raw.githubusercontent.com/AEndrix03/Graft/master/bucket/graft.json
```

Building from source (contributors, GPU builds, unsupported platforms):

```bash
git clone https://github.com/AEndrix03/Graft.git && cd Graft
bash scripts/build-from-source.sh          # pwsh scripts/build-from-source.ps1 on Windows
GRAFT_GPU=cuda bash scripts/build-from-source.sh   # or GRAFT_GPU=hip
```

Full installation reference → **[`docs/install/`](./docs/install/)**

---

## See it work

First, the memory is empty:

```console
$ graft query "spring validation nested dto not working"
{
  "status": 0,
  "result": { "hit": "MISS" }
}
```

The agent investigates and solves the issue. Save the useful part:

```bash
graft insert \
  --title "Spring @Valid must also be applied to nested DTO fields" \
  --body "Without @Valid on the nested field, validation does not cascade into it." \
  --keyword spring-boot \
  --keyword validation \
  --keyword gotcha
```

Weeks later, with different wording:

```console
$ graft query "why are constraints inside my nested request object ignored?"
{
  "status": 0,
  "result": {
    "hit": "STRONG",
    "title": "Spring @Valid must also be applied to nested DTO fields",
    "body": "Without @Valid on the nested field, validation does not cascade into it."
  }
}
```

Different prompt. Same underlying problem.

**Graft surfaces the prior learning. The agent decides whether it is useful.**

---

## One memory layer, several ways to use it

<table>
<tr>
<td width="33%" valign="top">

### Verified recall

`graft query`

Fast top-1 lookup with confidence gating:

`STRONG` · `WEAK` · `MISS`

Use it when the agent wants to know:

> Have I seen this before?

</td>
<td width="33%" valign="top">

### Hybrid retrieval

`graft retrieve`

Combines:

- BGE-M3 vectors
- BM25 title search
- BM25 body search
- Reciprocal Rank Fusion

Use it when several memories may help.

</td>
<td width="33%" valign="top">

### Graph exploration

`graft explore`

Walks semantic and keyword relationships with beam search, score decay and MMR diversity.

Use it when the agent wants to know:

> What else is connected to this?

</td>
</tr>
</table>

---

## Why not just a vector database?

Because Graft is shaped around **what an agent learns while working**, not around bulk document ingestion.

| | Vector DB / traditional RAG | Graft |
|---|---|---|
| Primary data | Documents | Agent learnings |
| Typical write | Bulk ingestion | Remember something useful |
| Typical read | Top-k chunks | Recall / retrieve / explore |
| Consumer | Application | AI agent |
| Confidence | Similarity ranking | `STRONG` / `WEAK` / `MISS` |
| Relationships | Usually external | Semantic + keyword graph |
| Knowledge changes | Replace/update documents | Supersession |
| Deployment | Database/service | Local binary + SQLite |

If you need to index millions of documents, use a vector database.

If you want your agent to remember **what it discovered while solving real problems**, Graft is built for that.

---

## Agent-native by design

Graft is a binary with a CLI contract. Any agent that can run a subprocess can use it.

| Agent | Integration | Setup |
|---|---|---|
| **Claude Code** | Skills | `graft setup` then `/graft-init` |
| **Codex** | Skills | `graft setup` then `/graft-init` |
| **Open Code** | Native skills | `graft setup` then `/graft-init` |
| **Gemini CLI** | `GEMINI.md` workflow | [`integrations/gemini-cli/`](./integrations/gemini-cli/) |
| **Claude Desktop** | MCP | [`integrations/claude-ai/`](./integrations/claude-ai/) |
| **ChatGPT** | MCP stdio / HTTP | [`integrations/chatgpt/`](./integrations/chatgpt/) |
| **Your agent** | CLI, subprocess, REST or MCP | [`docs/integrations/`](./docs/integrations/) |

The shipped integrations teach agents a simple pattern:

```text
non-trivial task
      │
      ▼
 search memory
      │
      ├── useful memory ───────► consider it
      │
      └── nothing useful ──────► solve normally
                                      │
                                      ▼
                              worth remembering?
                                      │
                                      ▼
                                   save it
```

For Claude Code, Graft includes skills such as:

- `recall` — smart search that escalates only when needed
- `memoryze` — distill useful learnings into reusable memories
- `learn` — intentionally ingest useful knowledge
- `memory-audit` — inspect memory quality and reuse

`/graft-init` writes the usage rule into your CLAUDE.md or AGENTS.md (and, on Claude Code, a rule file under `.claude/rules/`). It never touches hooks or agent settings files.

---

## Local by default

Graft keeps its core runtime on your machine:

```text
agent
  │
  ▼
graft CLI
  │
  │ MessagePack / AF_UNIX
  ▼
graftd
  │
  ├── SQLite + FTS5 + sqlite-vec
  │
  └── llama.cpp + BGE-M3
```

That means:

- one local database
- local embeddings
- no managed memory service
- no telemetry requirement
- no external API key
- CPU works out of the box
- CUDA / ROCm are optional

Chat clients can reach the same core through MCP.

```text
ChatGPT / Claude Desktop
          │
         MCP
          │
          ▼
    MCP adapter
          │
          ▼
      graft CLI
          │
          ▼
        graftd
```

---

## Three commands cover most workflows

### `query` — Do I already know this?

```bash
graft query "docker container exits after healthcheck"
```

Returns one confidence-gated result.

### `retrieve` — What relevant knowledge do I have?

```bash
graft retrieve "docker healthcheck networking"
```

Returns ranked memories using dense + lexical retrieval.

### `explore` — What is connected to this?

```bash
graft explore "deployment failures" --keyword docker
```

Walks the memory graph for broader context.

---

## Under the hood

### Recall

```text
query
  → BGE-M3 embedding
  → vector candidates
  → lexical verification
  → confidence gating
  → STRONG / WEAK / MISS
```

### Retrieval

```text
vector search ─┐
BM25 title ────┼─→ RRF → ranked memories
BM25 body ─────┘
```

### Explore

```text
semantic seed
  → graph edges
  → beam search
  → score decay
  → MMR diversity
```

The core is written in C11. Embeddings run locally through llama.cpp using BGE-M3. Storage is SQLite with FTS5 and sqlite-vec.

Graft itself does not require an external LLM call to store or retrieve memory.

---

## Memory that can evolve

A memory node contains:

```text
title
body
keywords
vector
relationships
status
```

Nodes can be connected through keyword and semantic edges.

When knowledge becomes outdated, Graft supports **supersession** rather than silently pretending the old knowledge never existed:

```text
old decision
     │
     └── SUPERSEDED BY ──► new decision
```

History stays inspectable while the newer memory becomes the useful one.

---

## Profiles

Separate memory spaces without running separate products:

```bash
GRAFT_PROFILE=work graft query "deployment rule"
GRAFT_PROFILE=personal graft query "docker workaround"
```

Profiles can be created, switched, exported, imported and merged.

```bash
graft profile list
graft profile add project-x
graft profile set project-x
```

This also gives you a straightforward way to move or combine local memory stores when needed.

---

## Inspect everything

The memory is not hidden behind a hosted platform.

```bash
graft stats
graft analytics
graft get <id>
graft delete <id>
```

Optional tooling includes:

- REST API
- MCP access
- browser graph viewer
- profile management
- usage analytics

---

## What Graft is not

**Not an LLM.**  
Your agent still reasons.

**Not a chatbot.**  
Bring your own agent.

**Not a hosted memory SaaS.**  
The default runtime is local.

**Not a vector database replacement.**  
It is opinionated around agent memory.

**Not just a semantic cache.**  
Verified reuse is one primitive. Graft also provides ranked retrieval, graph exploration, evolving memories and agent workflows.

---

## A secondary use case: semantic reuse in services

The same primitives can sit in front of an LLM-backed service:

```text
request
   │
   ▼
exact cache
   │ MISS
   ▼
Graft
   │ no useful memory
   ▼
LLM
   │
   └──► remember result
```

This is an **experimental design pattern**, not Graft's primary positioning.

See [`docs/microservices/`](./docs/microservices/).

---

## Project status

> **Active alpha — v0.1.x**

Working today:

- local daemon + CLI
- SQLite storage
- BGE-M3 embeddings
- verified recall
- hybrid retrieval
- graph exploration
- profiles
- Claude Code / Codex / Open Code skills
- MCP bridge
- optional REST API and graph viewer

Still evolving:

- API surface before 1.0
- packaging and platform coverage
- remote / shared memory
- team workflows
- neural reranking

The cross-encoder reranker is currently scaffolded but not active; verification currently relies on vector similarity plus lexical signals.

---

## Roadmap

**Now**

- harden CLI and JSON contracts
- improve coding-agent integrations
- improve memory quality and observability
- publish better benchmarks

**Next**

- BGE reranker
- contradiction detection
- adaptive thresholds
- richer hooks
- remote read-only profiles

**Later**

- shared team memory
- distributed profile sync
- automatic consolidation
- richer admin tooling

---

## Documentation

| | |
|---|---|
| **Getting started** | [`docs/install/`](./docs/install/) |
| **Use cases** | [`docs/use-cases.md`](./docs/use-cases.md) |\n| **Guide: persistent coding-agent memory** | [`docs/use-cases/persistent-memory-for-coding-agents.md`](./docs/use-cases/persistent-memory-for-coding-agents.md) |
| **Concepts** | [`docs/concepts.md`](./docs/concepts.md) |
| **Integrations** | [`docs/integrations/`](./docs/integrations/) |
| **Architecture** | [`docs/architecture/`](./docs/architecture/) |
| **CLI** | [`docs/cli/`](./docs/cli/) |
| **Retrieval** | [`docs/retrieval/`](./docs/retrieval/) |
| **Storage** | [`docs/storage/`](./docs/storage/) |
| **Embeddings** | [`docs/embeddings/`](./docs/embeddings/) |
| **Profiles** | [`docs/profiles/`](./docs/profiles/) |
| **HTTP API** | [`docs/http-api/`](./docs/http-api/) |

Full documentation → **[`docs/`](./docs/)**

---

## Contributing

```bash
git clone https://github.com/AEndrix03/Graft.git
cd Graft
bash scripts/build-from-source.sh
graft stats
```

Run tests with:

```bash
cmake --build build --target test
```

See [`CONTRIBUTING.md`](./CONTRIBUTING.md).

---

## License

[Apache License 2.0](./LICENSE).

You can use, modify, distribute and embed Graft in proprietary projects subject to the license terms.

---

<div align="center">

<img src="./assets/graft.png" alt="Graft" width="72"/>

### Let your agents keep what they learn.

**Local-first · Agent-native · No SaaS · No API key**

[`docs`](./docs/) · [`install`](./docs/install/) · [`integrations`](./docs/integrations/) · [`releases`](https://github.com/AEndrix03/Graft/releases) · [`issues`](https://github.com/AEndrix03/Graft/issues)

</div>
