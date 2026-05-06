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

### Opzione rapida (raccomandata)

```bash
bash scripts/install.sh        # Linux, macOS, Windows MSYS2
pwsh scripts/install.ps1       # Windows (autoinstalla MSYS2 se serve)
```

Lo script è interattivo, ti chiede solo l'essenziale (3 prompt totali) e fa tutto il resto in automatico: dipendenze di sistema, submodules, build di llama.cpp, download del modello BGE-M3 (~600 MB), build di memgraph e smoke test. È idempotente: ri-eseguirlo non ripete lavoro già fatto.

### Opzione manuale

#### 1. Prerequisiti

- **Linux/macOS**: `build-essential`/`Xcode`, `cmake ≥ 3.20`, `git`, `curl`, `pkg-config`, `libsqlite3-dev`, `libyaml-dev`.
- **Windows**: MSYS2 con MinGW64 (`gcc`, `pkg-config`, `mingw-w64-x86_64-libyaml`, `sqlite3`), `cmake`, `git`. Vedi `plans/sub_task_0_preambolo.md`.

#### 2. Clone & dipendenze

```bash
git clone <repo> agent-memory && cd agent-memory
# third_party già clonati nel repo: sqlite-vec, mpack, BLAKE3, llama.cpp
```

#### 3. Build di llama.cpp (una volta)

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

#### 4. Modello BGE-M3

```bash
mkdir -p models
curl -L --ssl-no-revoke -o models/bge-m3.gguf \
  "https://huggingface.co/lm-kit/bge-m3-gguf/resolve/main/bge-m3-Q8_0.gguf"
```

(~600 MB; quantization Q8_0.)

#### 5. Build memgraph

```bash
cmake -B build
cmake --build build
```

Output: `build/memgraph` (CLI) e `build/memgraphd` (daemon).

---

## Avvio

Il **CLI auto-avvia il daemon** se non è in esecuzione: il primo comando paga ~1-2s di cold-start, gli successivi sono veloci. Non serve fare nulla manualmente.

```bash
memgraph stats   # se il daemon è giù, viene avviato automaticamente
```

Per controllare/avviare manualmente (opzionale):

```bash
./build/memgraphd --config ./config.example.yaml &
```

Il daemon stampa `memgraphd: listening on /tmp/memgraph.sock`. Override:
- `MEMGRAPH_SOCKET` — path del socket (default per-profilo).
- `MEMGRAPH_CONFIG` — path del config file usato dall'auto-start (default `~/.lmemorygraph/config.yaml` se installato).
- `MEMGRAPH_HOME` — root directory (default `~/.lmemorygraph`).
- `MEMGRAPH_PROFILE` — profilo attivo (default `default`).
- `MEMGRAPH_USAGE_LOG` — path del log JSONL usato da `analytics` (default `<MEMGRAPH_HOME>/usage.jsonl`).

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

### Profili — più memorie isolate

Un profilo è una "tenant" del grafo: ognuno ha il proprio DB SQLite e il proprio daemon su un proprio socket. Il profilo `default` è creato automaticamente al primo avvio e non è rimovibile.

```bash
memgraph profile list
memgraph profile current

memgraph profile add work
memgraph profile add personal
memgraph profile remove personal           # chiede conferma (digitare il nome)

# Cambio profilo per la sessione corrente (env var di terminale):
eval "$(memgraph profile set work)"        # bash/zsh/fish
memgraph profile set work | iex            # PowerShell

# Per renderlo persistente, aggiungi la riga stampata al tuo shell rc
# (~/.bashrc, ~/.zshrc, profile.ps1...). Niente magia globale.

# Backup / migrazione:
memgraph profile export work --path work-2026-05-06.mgprofile
memgraph profile import --name work-restored --file work-2026-05-06.mgprofile
```

Risoluzione del profilo attivo: `$MEMGRAPH_PROFILE` → `default`. Niente file di stato globale: il `set` stampa solo l'export, sei tu che decidi se applicarlo alla sessione (`eval`) o renderlo persistente (shell rc).

I file vivono in:
- POSIX: `~/.lmemorygraph/profiles/<name>/memgraph.db` + `/tmp/memgraph-<name>.sock`
- Windows: `%USERPROFILE%\.lmemorygraph\profiles\<name>\memgraph.db` + `%USERPROFILE%\.lmemorygraph\sockets\<name>.sock`

Ogni profilo ha il suo daemon che si auto-avvia a richiesta.

### Analytics — sta valendo la pena?

```bash
memgraph analytics                       # finestra: tutto lo storico
memgraph analytics --since 7d            # ultima settimana
memgraph analytics --seconds-per-hit 90  # stima personalizzata del tempo risparmiato per cache hit
```

Riporta hit-rate (STRONG / total query), latenza media, rapporto insert/query, tempo stimato risparmiato e i top nodi più riusati. Operazione locale: legge solo `~/.memgraph/usage.jsonl` (o `MEMGRAPH_USAGE_LOG`), non contatta il daemon.

Letture chiave:
- `cache.hit_rate` < 0.10 → o stai cercando con la frase sbagliata, o stai inserendo entry troppo specifiche.
- `insert_to_query_ratio` > 1 → stai accumulando senza cercare prima.
- `top_reused_nodes` con conteggi alti → candidati naturali a diventare README/standard di team.

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
| Claude Code  | CLI   | `~/.claude/skills/{memgraph,recall,memoryze,learn,memory-audit}/` (5 skill) |
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

---

## Licenza

(Da definire.)
