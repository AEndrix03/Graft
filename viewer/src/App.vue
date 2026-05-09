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
    />

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
</style>
