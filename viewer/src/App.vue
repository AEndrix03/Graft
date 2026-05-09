<script setup>
import { ref, onMounted, onBeforeUnmount, computed } from 'vue';
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

async function onSearch({ mode, text, top_k, depth, beam }) {
  try {
    if (mode === 'match') {
      const r = unwrap(await api.match(text));
      if (r && r.id_hex) {
        selectedId.value = r.id_hex;
        highlightedIds.value = [r.id_hex];
      } else {
        highlightedIds.value = [];
      }
    } else if (mode === 'search') {
      const r = unwrap(await api.search(text, top_k));
      const ids = (r?.results || []).map((x) => x.id_hex);
      highlightedIds.value = ids;
      selectedId.value = ids[0] || null;
    } else if (mode === 'explore') {
      const r = unwrap(await api.explore(text, depth, beam));
      const ids = (r?.nodes || []).map((x) => x.id_hex);
      highlightedIds.value = ids;
      selectedId.value = ids[0] || null;
    }
  } catch (e) {
    loadError.value = e.message || String(e);
  }
}

function onClear() {
  highlightedIds.value = [];
  selectedId.value = null;
}

function onNodeSelected(id) {
  selectedId.value = id;
  highlightedIds.value = [id];
  edgeTooltip.value = null;
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
}

async function onPanelDeleted() {
  await refreshGraph();
  selectedId.value = null;
  highlightedIds.value = [];
}

onMounted(() => { refreshGraph(); startPolling(); });
onBeforeUnmount(stopPolling);

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
      @select="onNodeSelected"
      @select-edge="onEdgeSelected"
    />

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
      <SearchBar @search="onSearch" @clear="onClear" />
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
