# memgraph viewer

3D graph viewer for memgraph. Vue 3 + Vite + three.js + CodeMirror. Dark mode, color-coded edges, click-to-edit with atomic supersession on save.

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

The dev server proxies `/v1/*` to `http://127.0.0.1:9977`, so the daemon must be running with `http.enabled: true`.

## Auth

If your daemon has `http.api_key` set, store it in the browser's localStorage:

```js
localStorage.setItem('memgraph_api_key', 'your-key');
```

The viewer attaches it as `Authorization: Bearer <key>` to all `/v1/*` calls. Static assets (`/`, `/assets/*`) are unauthenticated.

---

## Visualization

- **Layout** — Each node is positioned in 3D via deterministic Rademacher random projection of its 1024-dim BGE-M3 embedding (server-side, in `/v1/view`). Coordinates are stable across reloads and recomputed each request but cheap.
- **Spread** — `SPATIAL_SCALE = 210` widens the scene so edges are visible as actual lines, not collisions.
- **Node size** — proportional to `body_len` (log-scaled): bigger sphere = longer content. Min/max bounded so dots don't disappear and giants don't dominate.
- **Node color** — hue is hashed (FNV-1a) from the node's primary keyword (alphabetically first), or its title if no keywords. Nodes sharing a keyword cluster visibly. Lit with `MeshLambertMaterial` + ambient + directional lights — soft Lambertian shading reads as "actual sphere", not flat disc.
- **Superseded nodes** — drawn in muted gray (`#5b6478`) regardless of keyword.
- **Labels** — CSS2D overlays parented to each sphere. **Hidden by default**, shown only when the camera is within `LABEL_NEAR_DIST = 8` of the node, or when the node is selected. Hidden completely on dimmed nodes (Match / Retrieve / Explore modes) to avoid clutter.

## Edges

Three kinds, color-coded:

| Kind         | Color        | Notes                                                      |
| ------------ | ------------ | ---------------------------------------------------------- |
| `semantic`   | lime         | Top-2 outgoing per source by default (visual de-clutter).  |
| `keyword`    | sky          | Top-2 outgoing per source.                                 |
| `supersedes` | red          | Always shown — load-bearing for history.                   |

- **Thickness scales with `weight`** — quartile-bucketed into 4 thickness levels (0.4 px → 1.8 px). Heavier edges read visibly thicker.
- **Top-2 filtering** is search-aware: in default view, edges between two highlighted nodes are kept regardless of the top-2 cap, so the relevant subgraph stays fully visible during search results.
- **Click an edge** to open a floating tooltip with kind + weight + keyword + ids.

The edge type toggle lives behind the gear icon (top right).

## Search modes

The mode selector in the search bar drives three different behaviors:

### Match (cache lookup)

Calls `/v1/match`. The verify pipeline returns one of:
- **STRONG** — exact-enough hit. Navigate to the node, dim everything else, no edges.
- **WEAK** — close-enough hit. Same as STRONG plus a banner: *"Weak match — similar but not exact. Verify the node before relying on it."*
- **MISS** — banner: *"Cache-miss — try a longer, more specific query."* No navigation, no dim.

### Retrieve (top-k hybrid)

Calls `/v1/search?top_k=N`. Returns the N best results by RRF score (vec + BM25 over title and body). The viewer:
- Dims every non-result node to 12% opacity.
- Colors results in a **5-tier red→orange ramp**:
  1. `#b30000` (vivid dark red) — most relevant
  2. `#e63b00`
  3. `#ff7f00`
  4. `#ffb84d`
  5. `#ffe0b3` (pale peach) — least relevant
- Renders only edges where **both endpoints are highlighted**. Semantic edges are still capped at top-2-per-source within the subgraph.
- Shows the **prev/next nav** + **score box** below the search bar:
  - `‹ N / Total ›` — keyboard shortcuts `←` / `→`.
  - `RRF XX.XX% · 0.0NNN` — percent is **absolute** against the theoretical RRF max `3/61 ≈ 0.0492`. So `100%` means "rank-1 in all three lists" (a strong match), not just "best of this batch."

Click on a result → navigates within the result list. Click on a dimmed node → exits Retrieve mode, single-node selection.

### Explore (graph walk)

Calls `/v1/explore?depth=N&beam=M&keywords=...`. Same dimming + color ramp as Retrieve, plus:
- Edges shown are **only the ones the algorithm traversed** — the actual walk path, not the surrounding subgraph. Drawn at a thicker fixed weight.
- Score box: `COS XX.XX% · 0.NNNN` — uses the bounded cosine-to-query (in `[-1, 1]`) instead of the unbounded log-additive beam composite.
- **Keyword chips**: type 3+ characters in the keyword input below the search bar to autocomplete from existing graph keywords. Click or `Enter` to add a chip; `Backspace` on empty input removes the last; click `×` to remove individually.
- Defaults: `depth=5`, `beam=1`. Wider beam (e.g. `beam=3 depth=4`) gives more coverage; `beam=1` gives a focused chain.

### Clear

Resets all search state — dim off, ramp off, nav hidden, chips kept. Re-enter normal view.

## Editing nodes

Click a node → right-side editor panel slides in:

- **Title** input.
- **Body** in a CodeMirror Markdown editor — line numbers, history, syntax highlight.
- **Keywords** — comma-separated, displayed below the editor.
- **Metadata** — author, created date (ISO 8601 UTC), expiration if set, full id.
- **State pill** — shows `active` / `superseded` / `stale`.

**Save** triggers an atomic supersession:
1. New node is inserted with the updated content.
2. Old node's state becomes `SUPERSEDED`.
3. A `SUPERSEDES` edge connects new → old.

The id changes; history is preserved. Match/Retrieve/Explore stop surfacing the old node, but it remains reachable by id (`/v1/nodes/{old_id}`).

The Save button is **disabled** when the current title/body/keywords match the loaded values exactly. Modify and revert → button disables again. **Delete** is a hard delete with cascading edge cleanup.

The editor panel is **resizable** — drag the left border. Width persists in localStorage between sessions. Limits: 360 px to 70vw.

## Live updates

The viewer polls `/v1/view` every 3s. Re-render fires only when `graph_version` (computed on the server as `n_nodes × 1e9 + n_edges`) changes — so an idle session doesn't repaint or recompute the layout each tick.

## Keyboard

| Key       | Action                                                                |
| --------- | --------------------------------------------------------------------- |
| `←` / `→` | Previous / next result (Retrieve and Explore, when 2+ results).       |
| Mouse drag| Orbit camera.                                                         |
| Scroll    | Zoom.                                                                 |
| Click     | Select node / open edge tooltip.                                      |

## What about CDN, hot reload, etc.

- The build is a single static bundle (`dist/index.html` + `dist/assets/*`). No CDN dependencies — the viewer works offline once built.
- For development, `npm run dev` starts Vite with HMR and proxies `/v1/*` to the running daemon.
- Bundle size is ~1 MB (~330 KB gzipped) — three.js + CodeMirror dominate. Code-splitting is on the roadmap when it actually matters.
