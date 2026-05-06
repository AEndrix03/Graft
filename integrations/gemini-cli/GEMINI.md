# memgraph — agent long-term memory

You have access to `memgraph`, a graph-based persistent memory CLI backed by `memgraphd` (running locally on `MEMGRAPH_SOCKET`, default `/tmp/memgraph.sock`).

## Use it BEFORE solving non-trivial problems

```
memgraph query "<concise restatement of what the user is asking>"
```

Read `result.hit`:
- `STRONG`: there's a near-exact match in memory. Cite and reuse: "Last time we hit this, we found that…".
- `WEAK`: similar; consider `memgraph get <id_hex>` for the full detail.
- `MISS`: see `result.fallback_retrieve.results[]` for related items.

For broader exploration:

```
memgraph retrieve "<text>" --top-k 10
memgraph explore  "<text>" --keyword K1 --keyword K2 --depth 3
```

## Use it AFTER successfully solving non-trivial problems

```
memgraph classify --summary "<one-line restatement>"          # suggested keywords
memgraph insert --summary "<title>" --detail "<solution + WHY>" \
                --keyword K1 --keyword K2 --keyword K3
```

Idempotent — re-saving the same content returns `"duplicate": true`.

## Skip memory for

Trivial tasks (typos, renames, syntax errors with obvious fixes), pure file operations, or content unsuitable for memory (secrets, chit-chat).

## Health check

```
memgraph stats
```

Errors? The daemon isn't running:
```
./build/memgraphd --config ./config.example.yaml &
```

## Output format

JSON-ish: `{"status": 0, "result": {…}}`. Non-zero status surfaces an `error` string.
