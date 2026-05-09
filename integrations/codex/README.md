# Codex — memgraph integration

Due livelli di integrazione:

- **AGENTS.md** — istruzioni statiche caricate dal modello come context. L'agent decide quando usare memgraph.
- **Hooks** (`hooks/`) — eseguiti dal harness in modo deterministico su `UserPromptSubmit` / `PostToolUse` / `Stop`. Non dipendono dal modello che si ricorda di usarli. Vedi [`hooks/README.md`](./hooks/README.md) per il setup (richiede il flag `[features] hooks = true` in `~/.codex/config.toml`).

## Install AGENTS.md

Codex (OpenAI's coding agent) reads `AGENTS.md` files at the repo root or in subdirs as context for the model.

```bash
# Repo-level (recommended)
cp integrations/codex/AGENTS.md ./AGENTS.md
# or append to an existing AGENTS.md:
cat integrations/codex/AGENTS.md >> ./AGENTS.md
```

## Daemon prerequisite

```bash
./build/memgraphd --config ./config.example.yaml &
export MEMGRAPH_SOCKET=/tmp/memgraph.sock
```

## Allow-listing

Codex may sandbox shell commands. Whitelist the `memgraph` binary in your Codex config so it's not prompted on every call.
