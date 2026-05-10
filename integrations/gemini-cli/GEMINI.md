# graft — agent long-term memory

You have access to `graft`, a graph-based persistent memory CLI backed by `graftd` (running locally on `GRAFT_SOCKET`, default `/tmp/graft.sock`).

## Use it BEFORE solving non-trivial problems

```
graft query "<concise restatement of what the user is asking>"
```

Read `result.hit`:
- `STRONG`: there's a near-exact match in memory. Cite and reuse: "Last time we hit this, we found that…".
- `WEAK`: similar; consider `graft get <id_hex>` for the full body.
- `MISS`: see `result.fallback_retrieve.results[]` for related items.

For broader exploration:

```
graft retrieve "<text>" --top-k 10
graft explore  "<text>" --keyword K1 --keyword K2 --depth 3
```

## Use it AFTER successfully solving non-trivial problems

```
graft classify --title "<one-line restatement>"          # suggested keywords
graft insert --title "<title>" --body "<solution + WHY>" \
                --keyword K1 --keyword K2 --keyword K3
```

Idempotent — re-saving the same content returns `"duplicate": true`.

## Skip memory for

Trivial tasks (typos, renames, syntax errors with obvious fixes), pure file operations, or content unsuitable for memory (secrets, chit-chat).

## Health check

```
graft stats
```

Errors? The daemon isn't running:
```
./build/graftd --config ./config.example.yaml &
```

## Output format

JSON-ish: `{"status": 0, "result": {…}}`. Non-zero status surfaces an `error` string.
