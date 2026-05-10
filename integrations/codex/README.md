# Codex — graft integration

Due livelli di integrazione:

- **AGENTS.md** — istruzioni statiche caricate dal modello come context. L'agent decide quando usare graft.
- **Hooks** (`hooks/`) — eseguiti dal harness in modo deterministico su `UserPromptSubmit` / `PostToolUse` / `Stop`. Non dipendono dal modello che si ricorda di usarli. Vedi [`hooks/README.md`](./hooks/README.md) per il setup (richiede il flag `[features] hooks = true` in `~/.codex/config.toml`).

## Install AGENTS.md

Installazione user-scoped automatica di istruzioni, skill compatibili e hook:

```bash
graft setup codex
```

Codex (OpenAI's coding agent) reads `AGENTS.md` files at the repo root or in subdirs as context for the model.

```bash
# Repo-level (recommended)
cp integrations/codex/AGENTS.md ./AGENTS.md
# or append to an existing AGENTS.md:
cat integrations/codex/AGENTS.md >> ./AGENTS.md
```

## Daemon prerequisite

```bash
./build/graftd --config ./config.example.yaml &
export GRAFT_SOCKET=/tmp/graft.sock
```

## Allow-listing

Codex may sandbox shell commands. Whitelist the `graft` binary in your Codex config so it's not prompted on every call.
