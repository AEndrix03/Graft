# Graft — Use Cases

This page shows concrete scenarios where graft adds value. Each one is based on a real workflow pattern that local AI agents encounter today.

---

## 1. Coding agent memory

**The problem.** You ask Claude Code to fix a bug. It finds the fix, explains it, applies it. Next session — same bug in a slightly different file — the agent starts from zero and re-derives the same fix.

**With graft.** After the first fix, the agent saves it:

```bash
graft insert \
  --title "Angular hydration mismatch caused by accessing window during SSR" \
  --body  "Wrap all browser-only logic in isPlatformBrowser(). \
window and document are not available during server rendering." \
  --keyword angular --keyword ssr --keyword hydration
```

Next time, any query that semantically matches — even with different phrasing — returns a `STRONG` hit instantly:

```bash
graft query "Angular SSR window is not defined error"
# → STRONG — title + body injected into agent context, no re-derivation needed
```

---

## 2. Remembering architectural decisions

**The problem.** You decide to use event sourcing instead of a state table. Three sprints later a new team member (or another agent session) proposes a state table. The decision has to be re-explained.

**With graft.**

```bash
graft insert \
  --title "We use event sourcing for order state, not a mutable status column" \
  --body  "Decision made 2025-04-12. Rationale: auditability for compliance, \
easier replay for analytics. A mutable status column would lose intermediate states." \
  --keyword architecture --keyword event-sourcing --keyword orders
```

Any future agent working on the order service will find this decision at `STRONG` confidence before touching the schema.

---

## 3. Reusing bug fixes

**The problem.** You spend two hours tracing a subtle bug — a race condition in your job queue, a misconfigured header, a framework quirk. Fixed. Closed. Forgotten.

**With graft.** The fix becomes a first-class memory:

```bash
graft insert \
  --title "Bull queue jobs silently dropped when Redis maxmemory-policy is volatile-lru" \
  --body  "If Redis evicts queue keys under memory pressure, Bull never retries \
and never raises an error. Fix: set maxmemory-policy to noeviction for the queue Redis instance." \
  --keyword redis --keyword bull --keyword queue --keyword production
```

When the same symptom appears six months later — on a different service, by a different agent — `graft query "bull jobs disappearing under load"` returns the exact fix.

---

## 4. Project-specific conventions

**The problem.** Your project has conventions that aren't in any README: naming rules, forbidden patterns, team preferences, deployment constraints. Agents infer them inconsistently.

**With graft.**

```bash
graft insert \
  --title "All service-to-service calls must go through the internal API gateway, not direct HTTP" \
  --body  "Direct HTTP bypasses auth, rate limiting, and the audit log. \
Use the InternalClient from @company/api-client. This applies to async jobs too." \
  --keyword convention --keyword architecture --keyword api-gateway

graft insert \
  --title "Do not add Lombok @Data to JPA entities — use explicit getters/setters" \
  --body  "@Data generates hashCode/equals on all fields, which breaks Hibernate \
proxy equality and causes LazyInitializationException in some contexts." \
  --keyword lombok --keyword jpa --keyword convention --keyword gotcha
```

---

## 5. Framework-specific troubleshooting

**The problem.** Some frameworks have non-obvious behaviours that take hours to diagnose but one line to fix. Stack Overflow posts go stale. Documentation misses the edge case.

**With graft.** Keep a personal troubleshooting library:

```bash
graft insert \
  --title "Spring Boot @Valid does not cascade to nested DTOs without @Valid on the field" \
  --body  "Add @Valid to the nested field in addition to the method parameter. \
@Validated on the controller is also required for method-level validation." \
  --keyword spring-boot --keyword validation --keyword gotcha

graft insert \
  --title "Docker build cache is invalidated if ARG appears before FROM" \
  --body  "ARG instructions before FROM are not cached per-stage. \
Move ARGs that don't affect the base image after FROM, or accept the cache miss." \
  --keyword docker --keyword build --keyword cache --keyword gotcha
```

`graft retrieve "spring validation not working"` surfaces these with context, keywords, and semantic links to related nodes.

---

## 6. Local semantic cache for microservices

**The problem.** Every request to your LLM-backed microservice calls the model, even when the answer hasn't meaningfully changed from a previous call.

**With graft.** Use the REST API as an L2 semantic cache:

```
Client → L1 Redis (exact match) → L2 graft (semantic match) → L3 LLM (generate + writeback)
```

A `GET /v1/match?text=<query>` returns STRONG/WEAK/MISS in 30–80 ms. STRONG answers skip the LLM entirely.
Every LLM-generated answer is written back via `POST /v1/insert`, so the cache warms itself over time.

Full pattern: [`docs/microservices/`](./microservices/).

---

## 7. Future: shared team memory

> **Planned — not available yet.**

Remote profiles will allow a team to expose a shared graft instance (read-only or read-write) over HTTP. Any agent on any machine can query the shared memory store, and local fixes can be pushed upstream.

This means bug fixes, architectural decisions, and troubleshooting knowledge discovered by one team member become immediately available to all agents across the team — without any manual documentation step.

Follow the [roadmap](../README.md#roadmap) for progress on remote profiles.

---

## CLI reference for these workflows

| Task | Command |
| ---- | ------- |
| Save a memory node | `graft insert --title S --body D --keyword K [--keyword K2...]` |
| Semantic cache lookup | `graft query "text"` |
| Top-k hybrid search | `graft retrieve "text" [--top-k N]` |
| Graph walk from keywords | `graft explore "text" [--keyword K] [--depth N]` |
| Suggest keywords for a title | `graft classify --title "text"` |
| Fetch a node by id | `graft get <hex_id>` |
| Delete a node | `graft delete <hex_id>` |
| Runtime statistics | `graft stats` |
| Hit-rate report | `graft analytics [--since 7d]` |
| Profile management | `graft profile <list\|add\|remove\|set\|export\|import>` |

Full CLI reference: [`docs/cli/`](./cli/).
