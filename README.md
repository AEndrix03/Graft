# memgraph

Memoria a grafo persistente per agenti AI. Salva, cerca e collega cose che hai imparato, per riusarle nelle conversazioni successive.

Stack: **C11** + SQLite + sqlite-vec + FTS5 + llama.cpp (BGE-M3, italiano) + MessagePack + AF_UNIX socket.

```
memgraph (CLI)  ──[unix socket]──▶  memgraphd (daemon)
                                     ├─ SQLite (nodes, edges, keywords, FTS5)
                                     ├─ sqlite-vec (cosine top-k 1024-dim)
                                     └─ llama.cpp + BGE-M3 (embedding)
```

---

## Installazione

### 1. Prerequisiti

- **Linux/macOS**: `build-essential`/`Xcode`, `cmake ≥ 3.20`, `git`, `curl`, `pkg-config`, `libsqlite3-dev`, `libyaml-dev`.
- **Windows**: MSYS2 con MinGW64 (`gcc`, `pkg-config`, `mingw-w64-x86_64-libyaml`, `sqlite3`), `cmake`, `git`. Vedi `plans/sub_task_0_preambolo.md`.

### 2. Clone & dipendenze

```bash
git clone <repo> agent-memory && cd agent-memory
# third_party già clonati nel repo: sqlite-vec, mpack, BLAKE3, llama.cpp
```

### 3. Build di llama.cpp (una volta)

```bash
cd third_party/llama.cpp
cmake -B build -DBUILD_SHARED_LIBS=ON -DGGML_NATIVE=ON \
               -DLLAMA_CURL=OFF -DLLAMA_BUILD_SERVER=OFF \
               -DLLAMA_BUILD_TOOLS=OFF -DLLAMA_BUILD_EXAMPLES=OFF \
               -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_COMMON=OFF \
               -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cd ../..
```

Su Windows aggiungi `-G "MinGW Makefiles"` al primo `cmake`.

### 4. Modello BGE-M3

```bash
mkdir -p models
curl -L --ssl-no-revoke -o models/bge-m3.gguf \
  "https://huggingface.co/lm-kit/bge-m3-gguf/resolve/main/bge-m3-Q8_0.gguf"
```

(~600 MB; quantization Q8_0.)

### 5. Build memgraph

```bash
cmake -B build
cmake --build build
```

Output: `build/memgraph` (CLI) e `build/memgraphd` (daemon).

---

## Avvio

```bash
./build/memgraphd --config ./config.example.yaml &
```

Il daemon stampa `memgraphd: listening on /tmp/memgraph.sock`. Override del socket via env `MEMGRAPH_SOCKET` (anche dal lato CLI).

Per fermarlo: `kill %1` o `Ctrl+C` se in foreground.

---

## Uso (CLI)

### Salvare conoscenza

```bash
memgraph insert \
  --summary "Spring Boot validazione cascade su DTO annidati" \
  --detail  "Serve @Valid sul campo annidato + @Validated sul controller. Senza @Valid, le constraint sui campi interni vengono ignorate." \
  --keyword spring-boot --keyword validazione --keyword jakarta-validation
```

Idempotente: re-inserire lo stesso `summary+detail+keywords` ritorna `"duplicate": true`.

### Cercare

```bash
# Cache lookup (best match)
memgraph query "validazione DTO Spring"
# → hit: STRONG | WEAK | MISS  (con fallback retrieve se MISS)

# Top-k ibrido (lexical + semantic via Reciprocal Rank Fusion)
memgraph retrieve "validazione DTO Spring" --top-k 10

# Walk del grafo
memgraph explore "validazione" --keyword spring-boot --depth 3 --beam 4
```

### Suggerire keyword

```bash
memgraph classify --summary "validazione di campi annidati Spring"
# → { "suggested_keywords": ["spring-boot","validazione",...] }
```

### Fetch puntuale

```bash
memgraph get <hex_id_32_chars>
```

### Stats

```bash
memgraph stats
# percentili p25/p50/p75/p90/p95/p99 della distribuzione di similarità
```

---

## Output

Tutti i comandi stampano JSON-ish:
```json
{ "status": 0, "result": { ... } }
```
- `status: 0` = OK.
- Status diverso da 0 → campo `error` con messaggio.
- Exit code: 0 OK, 1 errore di trasporto/encode, 3 handler ha ritornato status != 0.

---

## Integrazione con assistenti AI

Le configurazioni sono in `integrations/`. Linee guida operative:

| Assistente   | Tipo  | File                                                   |
| ------------ | ----- | ------------------------------------------------------ |
| Claude Code  | CLI   | `~/.claude/skills/memgraph/SKILL.md`                   |
| Codex        | CLI   | `~/.codex/AGENTS.md`                                   |
| Gemini CLI   | CLI   | `~/.gemini/GEMINI.md`                                  |
| Open Code    | CLI   | `~/.config/opencode/AGENTS.md`                         |
| Claude AI    | MCP   | `integrations/claude-ai/claude_desktop_config.json`    |
| ChatGPT      | MCP   | `integrations/chatgpt/mcp_config.json`                 |

Per Claude AI / ChatGPT serve anche il server MCP Python:
```bash
cd integrations/mcp-server && pip install -e .
```

Le **skill istruiscono l'LLM** a:
1. **Cercare prima di rispondere** (`memgraph query`) per problemi non banali.
2. **Salvare dopo** (`memgraph insert`) soluzioni nuove e non ovvie.
3. **Saltare** memoria per task triviali.

---

## Configurazione

`config.example.yaml` contiene i default. Da copiare in `config.yaml` per personalizzare.

Parametri chiave:
- `embedding.threads` (default 4)
- `cache.weak_hit_min_vec` (0.85) e `cache.strong_hit_min_lex` (0.15) — soglie del gating
- `retrieval.top_k` (25)
- `edges.edge_semantic_min` (0.6) — soglia per creare un arco semantico

---

## Architettura

- **Pipeline insert**: `embed(summary) → upsert keywords → vector_topk(query, kw) per archi keyword → vector_topk + MMR per archi semantici → INSERT atomico`.
- **Pipeline query**: `embed(text) → vec_topk(1) → trigram Jaccard + (cross-encoder opz.) → gating STRONG/WEAK/MISS`.
- **Pipeline retrieve**: 3 liste (vec, BM25 summary, BM25 detail) → RRF score → top-k.
- **Pipeline explore**: seed da vec_topk filtrato per keyword → beam search con MMR + decay `gamma^step`.

Dettagli per modulo: `plans/sub_task_*.md`.

---

## Roadmap

Fuori MVP, già strutturati come hook:
- Cross-encoder neurale (BGE reranker v2 m3) — flag `verification.cross_encoder_enabled`.
- NLI per contraddizioni → archi `MG_EDGE_CONTRADICTS`.
- Calibrazione adattiva delle soglie da `stats`.
- `consolidate` reale (dedup, supersede, stale-mark).
- API count in storage (ora `n_nodes`/`n_edges`/`n_keywords` ritornano 0 nello stats).

---

## Licenza

(Da definire.)
