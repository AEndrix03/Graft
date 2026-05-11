# 3D Viewer

A local browser app for **seeing** and **editing** your graft graph. Vue 3 + Vite + three.js + CodeMirror. Bundled as a static SPA and served by the daemon's HTTP layer.

The viewer is **not** required for any retrieval task — the CLI and the REST API are fully self-contained. The viewer exists because once a graph has more than a few hundred nodes, "have I solved this before?" is partly a navigation problem and partly a structure problem, and 3D is a surprisingly good fit for both.

```text
                            three.js scene
                            (live, orbit-able)
                            ┌─────────────────┐
HTTP/REST  ───►  /v1/view ─►│   nodes:        │
                            │     · color  → primary keyword hash
                            │     · size   → body length
                            │     · state  → muted for SUPERSEDED
                            │   edges:        │
                            │     · semantic → lime
                            │     · keyword  → sky
                            │     · supersedes → red (always shown)
                            └─────────────────┘
```

## Build

```bash
cd viewer
npm install
npm run build      # outputs viewer/dist/
```

The daemon's static handler serves `viewer/dist/` when `http.enabled: true`. Path is configurable via `http.viewer_path` (default `viewer/dist`). The Homebrew formula bundles a prebuilt copy under `$(brew --prefix graft)/viewer/` so brew installs get the viewer "for free".

`graft view` auto-builds the viewer on first run if it's not built yet, then opens your default browser at `http://127.0.0.1:9977/`.

## Dev (hot reload)

```bash
npm run dev        # http://localhost:5173, proxies /v1/* to 127.0.0.1:9977
```

The dev server needs the daemon running with `http.enabled: true` so the proxy has somewhere to send `/v1/*`.

## Visualisation rules

| Aspect | Rule |
| ------ | ---- |
| **Layout** | Each node placed in 3D via deterministic Rademacher random projection of its 1024-dim BGE-M3 embedding. Stable across reloads, recomputed each `/v1/view` call (cheap). |
| **Spread** | `SPATIAL_SCALE = 210` widens the scene so edges read as actual lines, not collisions. |
| **Node size** | Proportional to `body_len` (log-scaled). Bigger sphere = longer content. Min / max bounded so dots don't disappear and giants don't dominate. |
| **Node colour** | Hue hashed (FNV-1a) from the node's `primary_keyword` (alphabetically first), or its title if no keywords. Nodes sharing a keyword cluster visibly. Lit with `MeshLambertMaterial` + ambient + directional lights. |
| **Superseded nodes** | Drawn in muted gray (`#5b6478`). Their `SUPERSEDES` edge stays red so the chain is obvious. |
| **Labels** | CSS2D overlays parented to each sphere. **Hidden by default**, shown only when the camera is within `LABEL_NEAR_DIST = 8` of the node, or when the node is selected. Hidden completely on dimmed nodes (Match / Retrieve / Explore modes). |

## Edges

| Kind         | Colour  | Notes |
| ------------ | ------- | ----- |
| `semantic`   | lime    | Top-2 outgoing per source by default (visual de-clutter). |
| `keyword`    | sky     | Top-2 outgoing per source. |
| `supersedes` | red     | **Always** shown — load-bearing for history. |

- **Thickness** scales with `weight` — quartile-bucketed into 4 levels (0.4 px → 1.8 px). Heavier edges read visibly thicker.
- **Top-2 filtering is search-aware**: in default view, edges between two highlighted nodes are kept regardless of the cap, so the relevant subgraph stays fully visible during search.
- **Click an edge** → floating tooltip with kind, weight, keyword (if any), and ids.

The edge-type toggle is behind the gear icon (top right).

## Search modes

A mode selector lives in the search bar.

### Match (cache lookup)

Calls `/v1/match`.

- **STRONG** → navigate to the node, dim everything else, no edges. The node body becomes selectable.
- **WEAK**   → same as STRONG plus a banner: *"Weak match — similar but not exact. Verify before relying on it."*
- **MISS**   → banner: *"Cache-miss — try a longer, more specific query."* No navigation, no dimming.

### Retrieve (hybrid top-k)

Calls `/v1/search?top_k=N`. The viewer:

- Dims every non-result node to 12 % opacity.
- Colours results in a **5-tier red→orange ramp**:
  1. `#b30000` (vivid dark red) — most relevant
  2. `#e63b00`
  3. `#ff7f00`
  4. `#ffb84d`
  5. `#ffe0b3` (pale peach) — least relevant
- Renders only edges where **both endpoints are highlighted**. Semantic edges are still capped at top-2-per-source within the subgraph.
- Shows prev / next nav + score box: `‹ N / Total ›` (`←` / `→` shortcuts), then `RRF XX.XX% · 0.0NNN`. The percent is absolute against the theoretical RRF max `3/61 ≈ 0.0492`. So `100%` means "rank-1 in all three lists".

Click on a result → navigates within the list. Click on a dimmed node → exits Retrieve mode, single-node selection.

### Explore (beam search)

Calls `/v1/explore?depth=N&beam=M&keywords=...`. Same dimming + ramp as Retrieve, plus:

- Edges shown are **only the ones the algorithm traversed** — the actual walk path, not the surrounding subgraph. Drawn thicker.
- Score box: `COS XX.XX% · 0.NNNN` — uses the bounded cosine-to-query (in `[-1, 1]`) instead of the unbounded log-additive beam composite.
- **Keyword chips**: type 3+ characters in the keyword input to autocomplete from existing graph keywords. `Enter` adds a chip; `Backspace` on empty input removes the last; click `×` to remove individually.

Defaults: `depth=5`, `beam=1`. Wider beam (`beam=3 depth=4`) gives more coverage; `beam=1` gives a focused chain.

### Clear

Resets dim / ramp / nav. Keyword chips are preserved. Returns to normal view.

## Editing nodes

Click a node → the right-side editor panel slides in:

- **Title** input
- **Body** in a CodeMirror Markdown editor — line numbers, history, syntax highlight, theme matches dark UI
- **Keywords** — comma-separated list below the editor
- **Metadata** — author, created date (ISO 8601 UTC), expiration if set, full id
- **State pill** — `active` / `superseded` / `stale`

**Save** triggers an atomic supersession (`POST /v1/insert` with `supersedes`):

1. New node is inserted with the updated content and freshly built edges.
2. Old node's state becomes `SUPERSEDED`.
3. A red `SUPERSEDES` edge connects new → old in one transaction.

The id changes. History is preserved. Match / Retrieve / Explore stop surfacing the old node, but it remains reachable by id (`GET /v1/nodes/{old_id}`).

The Save button **disables** when current title / body / keywords match the loaded values exactly. Modify and revert → disables again.

**Delete** is a hard delete with cascading edge cleanup. It requires `endpoint_delete: true` on the daemon — if it's off, the button is hidden.

The editor panel is **resizable** — drag the left border. Width persists in `localStorage` between sessions. Limits: 360 px to 70 vw.

## Live updates

The viewer polls `/v1/view` every 3 s. Re-render fires **only** when `graph_version` (`n_nodes × 1e9 + n_edges`) changes. So an idle session does not repaint or recompute the layout each tick — even with the viewer open in a background tab.

## Keyboard

| Key       | Action |
| --------- | ------ |
| `←` / `→` | Previous / next result (Retrieve and Explore, when 2+ results). |
| Mouse drag | Orbit camera. |
| Scroll | Zoom. |
| Click | Select node / open edge tooltip. |

## What about CDN, hot reload, etc.

- The build is a single static bundle (`dist/index.html` + `dist/assets/*`). **No CDN dependencies** — the viewer works offline once built.
- For development, `npm run dev` starts Vite with HMR and proxies `/v1/*` to the daemon.
- Bundle size is ~1 MB (~330 KB gzipped) — three.js + CodeMirror dominate. Code-splitting is on the roadmap when it matters.

## Auth & exposure

The viewer is a **local / dev** surface. It talks to the local daemon REST API. For public deployments, put the OAuth/OIDC gateway in `integrations/mcp-server/` in front of `/v1/*` and keep `graftd` bound to `127.0.0.1`.

---

## What's missing and how to improve it

- **Diff view** when editing a node: side-by-side "old → new" before pressing Save. Today you press Save and trust supersession to preserve the old node.
- **Edge-filter UI**. The gear icon toggles edge types, but not edge thresholds. A slider for "show only edges with weight ≥ X" would help large graphs.
- **Bulk operations**. Click + shift-click + delete or click + shift-click + add-keyword would dramatically speed up cleanup.
- **2D fallback** for users who don't get along with 3D navigation. A flat force-directed graph would cover the same use cases on machines with poor GPUs.
- **A real onboarding overlay** for first-time users. Today the only documentation in-app is the gear menu; new users routinely miss the Retrieve / Explore modes.
- **Better empty-state.** With zero nodes the viewer shows an empty scene. A small "Welcome — here's how to insert your first node" panel would be friendlier.
