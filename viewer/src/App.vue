<script setup>
import { ref, onMounted, onBeforeUnmount, computed, watch } from 'vue';
import Graph3D from './components/Graph3D.vue';
import SearchBar from './components/SearchBar.vue';
import SettingsPanel from './components/SettingsPanel.vue';
import NodePanel from './components/NodePanel.vue';
import { api, unwrap } from './api.js';

const nodes = ref([]);
const edges = ref([]);
const graphVersion = ref(0);
const loadError = ref('');
const lastLoadedAt = ref(0);

const selectedId    = ref(null);
const highlightedIds = ref([]);
const showSemantic  = ref(true);
const showKeyword   = ref(true);
const edgeTooltip   = ref(null);  // { kind, weight, keyword?, x, y }
/* When true, non-highlighted nodes fade to a low opacity and edges are
 * restricted to the highlighted subgraph. Currently driven only by Retrieve. */
const dimMode       = ref(false);
const colorRamp     = ref(null);  // 'orange' or null — controls the rank coloring
const searchMode    = ref(null);  // 'match'|'search'|'explore' or null — for prev/next visibility
const searchScores  = ref(new Map());  // id_hex -> raw RRF score (Retrieve only)
const topScore      = ref(0);          // highest score in the current Retrieve result set
const pathEdges     = ref([]);         // edges traversed by Explore — drives the path overlay

/* All distinct keyword strings present in the live graph. We harvest them
 * from the keyword-kind edges in /v1/view so the autocomplete reflects
 * what's actually in the user's data, not a stale list. */
const allKeywords = computed(() => {
  const set = new Set();
  for (const e of edges.value) {
    if (e.kind === 'keyword' && e.keyword) set.add(e.keyword);
  }
  return [...set].sort();
});
const selectedIdx = computed(() => {
  if (!selectedId.value) return -1;
  return highlightedIds.value.indexOf(selectedId.value);
});
const selectedScore = computed(() => {
  if (!selectedId.value) return null;
  const s = searchScores.value.get(selectedId.value);
  return typeof s === 'number' ? s : null;
});
const selectedScorePct = computed(() => {
  if (selectedScore.value == null || topScore.value <= 0) return null;
  return (selectedScore.value / topScore.value) * 100;
});
const matchFeedback = ref(null);  // { hit: 'MISS'|'WEAK', tone: 'warn'|'info', text: string }
let matchFeedbackTimer = null;
function setMatchFeedback(fb) {
  matchFeedback.value = fb;
  if (matchFeedbackTimer) clearTimeout(matchFeedbackTimer);
  if (fb) matchFeedbackTimer = setTimeout(() => { matchFeedback.value = null; }, 8000);
}

let pollTimer = null;
const POLL_INTERVAL_MS = 3000;

async function refreshGraph() {
  try {
    const env = await api.view();
    const r = unwrap(env);
    if (!r) return;
    if (r.graph_version === graphVersion.value && nodes.value.length) return;
    graphVersion.value = r.graph_version;
    nodes.value = r.nodes || [];
    edges.value = r.edges || [];
    lastLoadedAt.value = Date.now();
    loadError.value = '';
  } catch (e) {
    loadError.value = e.message || String(e);
  }
}

function startPolling() {
  if (pollTimer) return;
  pollTimer = setInterval(refreshGraph, POLL_INTERVAL_MS);
}
function stopPolling() {
  if (pollTimer) { clearInterval(pollTimer); pollTimer = null; }
}

async function onSearch({ mode, text, top_k, depth, beam, keywords }) {
  try {
    if (mode === 'match') {
      const r = unwrap(await api.match(text));
      const hit = r?.hit;
      if (hit === 'STRONG' && r.id_hex) {
        selectedId.value = r.id_hex;
        highlightedIds.value = [r.id_hex];
        setMatchFeedback(null);
      } else if (hit === 'WEAK' && r.id_hex) {
        selectedId.value = r.id_hex;
        highlightedIds.value = [r.id_hex];
        setMatchFeedback({
          hit: 'WEAK',
          tone: 'info',
          text: 'Weak match — similar but not exact. Verify the node before relying on it.',
        });
      } else {
        highlightedIds.value = [];
        setMatchFeedback({
          hit: 'MISS',
          tone: 'warn',
          text: 'Cache-miss — try a longer, more specific query (English works best with the embedding model).',
        });
      }
    } else if (mode === 'search') {
      const r = unwrap(await api.search(text, top_k));
      const items = r?.results || [];
      const ids = items.map((x) => x.id_hex);
      highlightedIds.value = ids;
      selectedId.value = ids[0] || null;
      const map = new Map();
      for (const it of items) {
        if (it && typeof it.score === 'number') map.set(it.id_hex, it.score);
      }
      searchScores.value = map;
      topScore.value = items.length ? (items[0].score ?? 0) : 0;
      dimMode.value = ids.length > 0;
      colorRamp.value = 'orange';
      searchMode.value = 'search';
    } else if (mode === 'explore') {
      const r = unwrap(await api.explore(text, depth, beam, keywords));
      const items = r?.nodes || [];
      const ids = items.map((x) => x.id_hex);
      highlightedIds.value = ids;
      selectedId.value = ids[0] || null;
      /* Score map for the score-box (uses node.score from explore output). */
      const map = new Map();
      for (const it of items) {
        if (it && typeof it.score === 'number') map.set(it.id_hex, it.score);
      }
      searchScores.value = map;
      topScore.value = items.length ? (items[0].score ?? 0) : 0;
      /* Path edges = the actual traversal walk. Used as the only edges
       * rendered while in Explore mode so the user sees the path, not
       * the surrounding subgraph. */
      pathEdges.value = (r?.edges || []).map((e) => ({
        src: e.src_hex || e.src,
        dst: e.dst_hex || e.dst,
        kind: e.kind || 'semantic',
        weight: e.weight ?? 1.0,
      }));
      dimMode.value = ids.length > 0;
      colorRamp.value = 'orange';
      searchMode.value = 'explore';
    }
  } catch (e) {
    loadError.value = e.message || String(e);
  }
}

function onClear() {
  highlightedIds.value = [];
  selectedId.value = null;
  setMatchFeedback(null);
  dimMode.value = false;
  colorRamp.value = null;
  searchMode.value = null;
  searchScores.value = new Map();
  topScore.value = 0;
  pathEdges.value = [];
}

function navigateResult(delta) {
  const ids = highlightedIds.value;
  if (!ids.length) return;
  const cur = selectedIdx.value;
  let next;
  if (cur < 0) next = 0;
  else next = (cur + delta + ids.length) % ids.length;
  selectedId.value = ids[next];
}

function onNodeSelected(id) {
  edgeTooltip.value = null;
  /* In Retrieve / Explore mode the highlighted set IS the result list.
   * Clicking a node already in that list just navigates within results
   * (preserves the dimmed scene + path edges). Clicking a non-result
   * node exits the mode entirely — same effect as Clear, but keeps the
   * new selection so the user can inspect what they actually clicked. */
  if (searchMode.value === 'search' || searchMode.value === 'explore') {
    if (highlightedIds.value.includes(id)) {
      selectedId.value = id;
      return;
    }
    dimMode.value = false;
    colorRamp.value = null;
    searchMode.value = null;
    pathEdges.value = [];
  }
  selectedId.value = id;
  highlightedIds.value = [id];
}

function onEdgeSelected(e) {
  edgeTooltip.value = {
    kind: e.kind,
    weight: e.weight,
    keyword: e.keyword,
    src: e.src,
    dst: e.dst,
    x: e.screenX,
    y: e.screenY,
  };
}

function onPanelClose() {
  selectedId.value = null;
}

async function onPanelSaved(newId) {
  await refreshGraph();
  if (newId) {
    selectedId.value = newId;
    highlightedIds.value = [newId];
  } else {
    selectedId.value = null;
  }
  dimMode.value = false;
  colorRamp.value = null;
  searchMode.value = null;
}

async function onPanelDeleted() {
  await refreshGraph();
  selectedId.value = null;
  highlightedIds.value = [];
}

function onKeydown(ev) {
  /* Don't intercept when an input/textarea/CodeMirror is focused. */
  const t = ev.target;
  if (t && (t.tagName === 'INPUT' || t.tagName === 'TEXTAREA' || t.isContentEditable)) return;
  if ((searchMode.value !== 'search' && searchMode.value !== 'explore') || highlightedIds.value.length < 2) return;
  if (ev.key === 'ArrowRight') { navigateResult(1); ev.preventDefault(); }
  else if (ev.key === 'ArrowLeft') { navigateResult(-1); ev.preventDefault(); }
}

onMounted(() => {
  refreshGraph();
  startPolling();
  window.addEventListener('keydown', onKeydown);
});
onBeforeUnmount(() => {
  stopPolling();
  window.removeEventListener('keydown', onKeydown);
});

const stats = computed(() => `${nodes.value.length} nodes · ${edges.value.length} edges`);
</script>

<template>
  <div class="app">
    <Graph3D
      class="canvas"
      :nodes="nodes"
      :edges="edges"
      :selected-id="selectedId"
      :highlighted-ids="highlightedIds"
      :show-semantic="showSemantic"
      :show-keyword="showKeyword"
      :dim-non-highlighted="dimMode"
      :color-ramp="colorRamp"
      :path-edges="pathEdges"
      @select="onNodeSelected"
      @select-edge="onEdgeSelected"
    />

    <transition name="fade-down">
      <div v-if="(searchMode === 'search' || searchMode === 'explore') && highlightedIds.length > 0" class="result-nav-wrap">
        <div v-if="highlightedIds.length > 1" class="result-nav">
          <button class="nav-btn" @click="navigateResult(-1)" title="Previous (←)">‹</button>
          <span class="counter">
            <span class="rank">{{ selectedIdx >= 0 ? selectedIdx + 1 : '–' }}</span>
            <span class="sep">/</span>
            <span class="total">{{ highlightedIds.length }}</span>
          </span>
          <button class="nav-btn" @click="navigateResult(1)" title="Next (→)">›</button>
        </div>
        <div v-if="selectedScore != null" class="score-box">
          <span class="score-label">score</span>
          <span class="score-pct">{{ selectedScorePct.toFixed(0) }}%</span>
          <span class="score-sep">·</span>
          <span class="score-abs">{{ selectedScore.toFixed(4) }}</span>
        </div>
      </div>
    </transition>

    <div
      v-if="edgeTooltip"
      class="edge-tooltip"
      :style="{ left: edgeTooltip.x + 'px', top: edgeTooltip.y + 'px' }"
      @click="edgeTooltip = null"
    >
      <div class="row">
        <span class="kind" :class="edgeTooltip.kind">{{ edgeTooltip.kind }}</span>
        <span class="weight">w = {{ edgeTooltip.weight.toFixed(3) }}</span>
      </div>
      <div v-if="edgeTooltip.keyword" class="kw">#{{ edgeTooltip.keyword }}</div>
      <div class="ids">
        <span class="mono">{{ edgeTooltip.src.slice(0, 8) }}…</span>
        <span class="arrow">→</span>
        <span class="mono">{{ edgeTooltip.dst.slice(0, 8) }}…</span>
      </div>
      <div class="hint">click to dismiss</div>
    </div>

    <div class="search-wrap">
      <SearchBar :all-keywords="allKeywords" @search="onSearch" @clear="onClear" />
      <transition name="fade-down">
        <div v-if="matchFeedback" class="match-banner" :class="matchFeedback.tone">
          <span class="badge" :class="matchFeedback.hit.toLowerCase()">{{ matchFeedback.hit }}</span>
          <span class="msg">{{ matchFeedback.text }}</span>
          <button class="dismiss" @click="setMatchFeedback(null)">×</button>
        </div>
      </transition>
    </div>

    <div class="settings-wrap">
      <SettingsPanel
        v-model:showSemantic="showSemantic"
        v-model:showKeyword="showKeyword"
      />
    </div>

    <div class="status">
      <span class="brand">memgraph</span>
      <span class="dim">·</span>
      <span class="stats">{{ stats }}</span>
      <span v-if="loadError" class="err">· {{ loadError }}</span>
    </div>

    <NodePanel
      v-if="selectedId"
      :node-id="selectedId"
      class="node-panel"
      @close="onPanelClose"
      @saved="onPanelSaved"
      @deleted="onPanelDeleted"
    />
  </div>
</template>

<style scoped>
.app {
  position: relative;
  width: 100%;
  height: 100vh;
  overflow: hidden;
}
.canvas { position: absolute; inset: 0; }
.search-wrap {
  position: absolute;
  top: 16px;
  left: 50%;
  transform: translateX(-50%);
  z-index: 10;
  width: min(720px, 80vw);
}
.settings-wrap {
  position: absolute;
  top: 16px;
  right: 16px;
  z-index: 10;
}
.status {
  position: absolute;
  bottom: 12px;
  left: 16px;
  z-index: 5;
  font-size: 12px;
  color: var(--text-muted);
  background: var(--bg-overlay);
  border: 1px solid var(--border-soft);
  padding: 6px 12px;
  border-radius: var(--radius-sm);
  backdrop-filter: blur(8px);
  display: flex;
  gap: 8px;
}
.brand { color: var(--text); font-weight: 500; }
.dim { color: var(--text-muted); }
.err { color: #f87171; }
.node-panel {
  position: absolute;
  top: 0;
  right: 0;
  z-index: 20;
}

.result-nav-wrap {
  position: absolute;
  top: 76px;
  left: 50%;
  transform: translateX(-50%);
  z-index: 9;
  display: flex;
  align-items: center;
  gap: 8px;
}
.result-nav, .score-box {
  display: flex;
  align-items: center;
  /* Brighter than the global overlay so these read at a glance */
  background: rgba(34, 38, 50, 0.95);
  border: 1px solid rgba(122, 162, 247, 0.32);
  border-radius: 999px;
  backdrop-filter: blur(8px);
  box-shadow: 0 6px 20px rgba(0, 0, 0, 0.45), 0 0 0 1px rgba(122, 162, 247, 0.05);
  font-family: var(--mono);
  font-size: 13px;
  color: var(--text);
}
.result-nav { gap: 6px; padding: 4px 10px; }
.score-box  { gap: 6px; padding: 6px 12px; }

.result-nav .nav-btn {
  width: 28px;
  height: 28px;
  display: flex;
  align-items: center;
  justify-content: center;
  background: transparent;
  border: 1px solid transparent;
  border-radius: 999px;
  font-size: 18px;
  line-height: 1;
  color: var(--text);
  cursor: pointer;
  padding: 0;
}
.result-nav .nav-btn:hover {
  background: rgba(122, 162, 247, 0.14);
  border-color: rgba(122, 162, 247, 0.45);
}
.result-nav .counter { padding: 0 4px; min-width: 56px; text-align: center; }
.result-nav .rank { color: #ff8c4d; font-weight: 700; }
.result-nav .sep { color: var(--text-muted); margin: 0 4px; }
.result-nav .total { color: var(--text-dim); }

.score-box .score-label {
  font-family: var(--font);
  font-size: 10px;
  text-transform: uppercase;
  letter-spacing: 0.08em;
  color: var(--text-muted);
  margin-right: 2px;
}
.score-box .score-pct { color: #ff8c4d; font-weight: 700; }
.score-box .score-sep { color: var(--text-muted); }
.score-box .score-abs { color: var(--text-dim); }

.match-banner {
  margin-top: 8px;
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 8px 12px;
  font-size: 12.5px;
  background: rgba(34, 38, 50, 0.95);
  border: 1px solid rgba(122, 162, 247, 0.28);
  border-radius: var(--radius);
  backdrop-filter: blur(8px);
  box-shadow: 0 6px 20px rgba(0, 0, 0, 0.45);
  color: var(--text);
}
.match-banner.warn { border-color: rgba(249, 226, 175, 0.5); }
.match-banner.info { border-color: rgba(122, 162, 247, 0.5); }
.match-banner .badge {
  font-size: 10px;
  font-weight: 600;
  text-transform: uppercase;
  letter-spacing: 0.06em;
  padding: 3px 8px;
  border-radius: 999px;
}
.match-banner .badge.miss {
  background: rgba(249, 226, 175, 0.16);
  color: #f9e2af;
  border: 1px solid rgba(249, 226, 175, 0.5);
}
.match-banner .badge.weak {
  background: rgba(122, 162, 247, 0.16);
  color: var(--accent);
  border: 1px solid rgba(122, 162, 247, 0.5);
}
.match-banner .msg { flex: 1; line-height: 1.45; }
.match-banner .dismiss {
  background: transparent;
  border: none;
  color: var(--text-dim);
  font-size: 18px;
  line-height: 1;
  padding: 0 4px;
  cursor: pointer;
}
.match-banner .dismiss:hover { color: var(--text); }

.fade-down-enter-active, .fade-down-leave-active {
  transition: opacity 0.18s ease, transform 0.18s ease;
}
.fade-down-enter-from {
  opacity: 0;
  transform: translateY(-6px);
}
.fade-down-leave-to {
  opacity: 0;
  transform: translateY(-6px);
}

.edge-tooltip {
  position: fixed;
  z-index: 30;
  transform: translate(12px, 12px);
  background: var(--bg-overlay);
  border: 1px solid var(--border);
  border-radius: var(--radius-sm);
  padding: 8px 10px;
  font-size: 12px;
  color: var(--text);
  backdrop-filter: blur(8px);
  box-shadow: var(--shadow);
  cursor: pointer;
  min-width: 200px;
}
.edge-tooltip .row {
  display: flex;
  justify-content: space-between;
  align-items: center;
  gap: 12px;
}
.edge-tooltip .kind {
  font-size: 10px;
  text-transform: uppercase;
  letter-spacing: 0.06em;
  padding: 2px 6px;
  border-radius: 3px;
  background: rgba(122,162,247,0.12);
  color: var(--accent);
}
.edge-tooltip .kind.semantic   { background: rgba(190,242,100,0.14); color: var(--edge-semantic); }
.edge-tooltip .kind.keyword    { background: rgba(125,211,252,0.14); color: var(--edge-keyword); }
.edge-tooltip .kind.supersedes { background: rgba(248,113,113,0.16); color: var(--edge-supersedes); }
.edge-tooltip .weight { color: var(--text-dim); font-family: var(--mono); }
.edge-tooltip .kw {
  margin-top: 4px;
  font-family: var(--mono);
  font-size: 11px;
  color: var(--edge-keyword);
}
.edge-tooltip .ids {
  margin-top: 4px;
  display: flex;
  gap: 6px;
  align-items: center;
  color: var(--text-muted);
}
.edge-tooltip .mono { font-family: var(--mono); font-size: 11px; }
.edge-tooltip .arrow { color: var(--text-muted); }
.edge-tooltip .hint {
  margin-top: 6px;
  font-size: 10px;
  color: var(--text-muted);
}
</style>
