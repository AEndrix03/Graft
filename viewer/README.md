# memgraph viewer

3D graph viewer for memgraph. Vue 3 + Vite + three.js + CodeMirror. Dark mode, keyword/semantic edges color-coded, click-to-edit with atomic supersession on save.

## Build

```bash
npm install
npm run build      # output: viewer/dist/
```

The daemon's static handler picks up `viewer/dist/` at runtime. Path is configurable via `http.viewer_path` in `config.yaml` (default: `viewer/dist`).

## Dev (hot reload)

```bash
npm run dev        # serves on http://localhost:5173
```

The dev server proxies `/v1/*` to `http://127.0.0.1:9977`, so you need the daemon running with `http.enabled: true`.

## Auth

If your daemon has `http.api_key` set, store it in the browser's localStorage:

```js
localStorage.setItem('memgraph_api_key', 'your-key');
```

The viewer attaches it as `Authorization: Bearer <key>` to all `/v1/*` calls. Static assets (`/`, `/assets/*`) are served unauthenticated.

## What it does

- **Layout** — Each node is positioned in 3D via deterministic Rademacher random projection of its 1024-dim BGE-M3 embedding (server-side, in `/v1/view`). Coordinates are stable across reloads.
- **Edges** — Coloured by kind: lime = semantic, sky = keyword, red = supersedes (always shown). Toggle semantic/keyword in the Settings panel (gear icon, top right).
- **Search** — Three modes:
  - **Match**: cache lookup, highlights the single STRONG/WEAK hit if any.
  - **Retrieve**: hybrid top-k via RRF, highlights all results.
  - **Explore**: graph walk with depth + beam, highlights walked nodes.
- **Edit** — Click a node to open the right panel. Edit title / body (Markdown) / keywords. **Save** does an atomic supersession: a new node is inserted with the updated content + a `SUPERSEDES` edge to the old one, the old node's state is set to `superseded`. The id changes, history is preserved.
- **Live updates** — The graph polls `/v1/view` every 3s and only re-renders when `graph_version` (n_nodes × 1e9 + n_edges) changes.
