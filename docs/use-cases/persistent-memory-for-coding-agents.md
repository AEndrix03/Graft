# Persistent memory for coding agents

A coding agent can solve a difficult problem today and rediscover the same answer next week.

The issue is not that the model cannot reason. It is that useful project knowledge is scattered across terminal output, discarded chat context, old branches and a developer's memory.

Graft is a local memory layer for the part worth keeping.

## The workflow

1. An agent encounters a non-trivial task.
2. Before investigating, it asks whether the project already contains a relevant learning.
3. It uses the result as context—not as an instruction it must blindly follow.
4. After solving the task, it stores the durable insight: the cause, decision, constraint or failed approach.

```text
new task
   │
   ▼
search Graft
   │
   ├── relevant prior learning ──► consider it
   │
   └── no useful memory ─────────► investigate normally
                                       │
                                       ▼
                              save the reusable learning
```

The result is not a larger chat history. It is a small, queryable body of knowledge distilled from work that was expensive to do.

## What belongs in memory?

Good memories answer a question that a future agent could otherwise spend time rediscovering.

| Worth remembering | Usually not worth remembering |
|---|---|
| A root cause and its fix | Raw terminal logs |
| Why an architectural decision was made | A complete conversation transcript |
| A project-specific framework gotcha | A temporary task checklist |
| A dependency constraint | Generic documentation available anywhere |
| An approach that failed and why | Unverified speculation |

A useful memory is concise, specific and gives enough evidence for the next agent to judge whether it applies.

## Example: a recurring validation bug

An agent finds that constraints inside a nested Spring request DTO are ignored. The final learning is not “I changed some code.” It is:

```bash
graft insert \
  --title "Spring @Valid must also be applied to nested DTO fields" \
  --body "Without @Valid on the nested field, validation does not cascade into it." \
  --keyword spring-boot \
  --keyword validation \
  --keyword gotcha
```

Weeks later, an agent asks in different words:

```bash
graft query "why are constraints inside my nested request object ignored?"
```

Graft can surface the learning with a confidence-gated result. The agent still evaluates it against the current codebase.

## Why not put everything in a vector database?

A vector database is excellent for document retrieval at scale. Graft is designed for a different unit of knowledge: an **agent learning** created while work is being done.

That means it supports:

- a fast top-one lookup with `STRONG`, `WEAK` and `MISS` gating;
- hybrid retrieval that combines local embeddings and lexical search;
- graph exploration for related decisions and constraints;
- supersession when an old decision becomes obsolete;
- local profiles for separate projects or contexts.

It runs as a local binary and daemon using SQLite, FTS5, sqlite-vec, llama.cpp and BGE-M3. No hosted memory service, account or embedding API key is required.

## Start small

Do not try to ingest an entire repository history on day one.

Start with the next few learnings that were genuinely hard-won:

- an incident diagnosis;
- a non-obvious build or deployment fix;
- an architectural tradeoff;
- a library integration trap.

Then make the agent search before substantial investigation and save only knowledge that would help a future session.

## Try it

Install Graft, then use `query` before a non-trivial task and `insert` after a useful discovery.

- [Installation](../install/)
- [Integrations](../integrations/)
- [Concepts](../concepts.md)
- [Graft README](../../README.md)
