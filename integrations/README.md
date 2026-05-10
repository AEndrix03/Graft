# integrations/

Adapter layer per esporre `graft` (CLI + daemon) verso assistenti AI.

Due famiglie di integrazione:

| Tipo     | Tool                | Meccanismo                                | Dir                          |
| -------- | ------------------- | ----------------------------------------- | ---------------------------- |
| **CLI**  | Claude Code         | Skill (`SKILL.md` con frontmatter)        | `claude-code/`               |
| **CLI**  | Codex (OpenAI)      | `AGENTS.md` istruzioni di repo            | `codex/`                     |
| **CLI**  | Open Code           | `AGENTS.md` istruzioni                    | `opencode/`                  |
| **CLI**  | Gemini CLI          | `GEMINI.md` memory file                   | `gemini-cli/`                |
| **CHAT** | Claude AI (desktop) | MCP server + `claude_desktop_config.json` | `claude-ai/` + `mcp-server/` |
| **CHAT** | ChatGPT             | MCP server + connector config             | `chatgpt/`   + `mcp-server/` |

## Architettura

```
┌─────────────────────────┐    ┌─────────────────────────┐
│ LLM (Claude / GPT / …)  │    │ Assistant CLI (Claude   │
│  via chat                │    │  Code, Codex, …)        │
└────────────┬────────────┘    └────────────┬────────────┘
             │ MCP (stdio)                  │ subprocess
             ▼                              ▼
┌─────────────────────────┐    ┌─────────────────────────┐
│ mcp-server/server.py    │    │ graft (CLI binary)   │
│  (wraps graft CLI)   │───▶│  → unix socket          │
└─────────────────────────┘    └────────────┬────────────┘
                                             ▼
                                ┌─────────────────────────┐
                                │ graftd (daemon)      │
                                │  SQLite + sqlite-vec    │
                                │  + BGE-M3 (llama.cpp)   │
                                └─────────────────────────┘
```

I CLI assistant (Claude Code, Codex, …) chiamano direttamente il binario `graft`. I client web/desktop (Claude AI, ChatGPT) parlano con `mcp-server/server.py` via stdio JSON-RPC, che a sua volta esegue subprocess di `graft`.

## Prerequisiti

1. Binari buildati: `build/graft` e `build/graftd`.
2. `graftd --config config.example.yaml` deve essere in esecuzione.
3. La env var `GRAFT_SOCKET` punta al socket (default `/tmp/graft.sock`).

## Setup CLI assistant

Per installare skill/istruzioni e hook nel profilo utente dell'assistant:

```bash
graft setup claudecode
graft setup codex
```

## Mapping operazioni → tool name

Tutti gli adapter espongono lo **stesso set di operazioni** sotto nomi consistenti:

| Sub-comando CLI    | MCP tool name        | Cosa fa                                   |
| ------------------ | -------------------- | ----------------------------------------- |
| `insert`           | `graft_insert`    | Salva un nuovo nodo (title+body)    |
| `query`            | `graft_query`     | Cache lookup multi-segnale (STRONG/WEAK/MISS) |
| `retrieve`         | `graft_retrieve`  | RRF lexical+semantic, top-k risultati     |
| `explore`          | `graft_explore`   | Beam search keyword-conditioned           |
| `get`              | `graft_get`       | Fetch nodo per id_hex                     |
| `classify`         | `graft_classify`  | Suggerisci keyword da un title          |
| `stats`            | `graft_stats`     | Percentili distribuzione + counts         |

## Quando usare cosa (linea guida per LLM)

- **Cerca prima di scrivere**: chiama sempre `query` (o `retrieve`) prima di `insert`. Se HIT STRONG → riutilizza, se MISS → eventualmente inserisci.
- **`query` vs `retrieve`**: `query` cerca cache esatta (1 candidato top con gating); `retrieve` ritorna top-k.
- **`explore` per problemi correlati**: quando l'utente chiede "cosa so su X / cosa è correlato a X".
- **`classify`** prima di `insert` se l'utente non ti passa keyword esplicite.
