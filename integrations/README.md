# integrations/

Adapter layer per esporre `memgraph` (CLI + daemon) verso assistenti AI.

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
│ mcp-server/server.py    │    │ memgraph (CLI binary)   │
│  (wraps memgraph CLI)   │───▶│  → unix socket          │
└─────────────────────────┘    └────────────┬────────────┘
                                             ▼
                                ┌─────────────────────────┐
                                │ memgraphd (daemon)      │
                                │  SQLite + sqlite-vec    │
                                │  + BGE-M3 (llama.cpp)   │
                                └─────────────────────────┘
```

I CLI assistant (Claude Code, Codex, …) chiamano direttamente il binario `memgraph`. I client web/desktop (Claude AI, ChatGPT) parlano con `mcp-server/server.py` via stdio JSON-RPC, che a sua volta esegue subprocess di `memgraph`.

## Prerequisiti

1. Binari buildati: `build/memgraph` e `build/memgraphd`.
2. `memgraphd --config config.example.yaml` deve essere in esecuzione.
3. La env var `MEMGRAPH_SOCKET` punta al socket (default `/tmp/memgraph.sock`).

## Mapping operazioni → tool name

Tutti gli adapter espongono lo **stesso set di operazioni** sotto nomi consistenti:

| Sub-comando CLI    | MCP tool name        | Cosa fa                                   |
| ------------------ | -------------------- | ----------------------------------------- |
| `insert`           | `memgraph_insert`    | Salva un nuovo nodo (sintetico+detail)    |
| `query`            | `memgraph_query`     | Cache lookup multi-segnale (STRONG/WEAK/MISS) |
| `retrieve`         | `memgraph_retrieve`  | RRF lexical+semantic, top-k risultati     |
| `explore`          | `memgraph_explore`   | Beam search keyword-conditioned           |
| `get`              | `memgraph_get`       | Fetch nodo per id_hex                     |
| `classify`         | `memgraph_classify`  | Suggerisci keyword da un summary          |
| `stats`            | `memgraph_stats`     | Percentili distribuzione + counts         |

## Quando usare cosa (linea guida per LLM)

- **Cerca prima di scrivere**: chiama sempre `query` (o `retrieve`) prima di `insert`. Se HIT STRONG → riutilizza, se MISS → eventualmente inserisci.
- **`query` vs `retrieve`**: `query` cerca cache esatta (1 candidato top con gating); `retrieve` ritorna top-k.
- **`explore` per problemi correlati**: quando l'utente chiede "cosa so su X / cosa è correlato a X".
- **`classify`** prima di `insert` se l'utente non ti passa keyword esplicite.
