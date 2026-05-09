<script setup>
import { ref } from 'vue';

const props = defineProps({
  showSemantic: Boolean,
  showKeyword:  Boolean,
});
const emit = defineEmits(['update:showSemantic', 'update:showKeyword']);
const open = ref(false);
</script>

<template>
  <div class="settings">
    <button class="trigger" :class="{ active: open }" @click="open = !open" title="Settings">
      <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
        <circle cx="12" cy="12" r="3"></circle>
        <path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 1 1-2.83 2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 1 1-4 0v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 1 1-2.83-2.83l.06-.06a1.65 1.65 0 0 0 .33-1.82 1.65 1.65 0 0 0-1.51-1H3a2 2 0 1 1 0-4h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 1 1 2.83-2.83l.06.06a1.65 1.65 0 0 0 1.82.33H9a1.65 1.65 0 0 0 1-1.51V3a2 2 0 1 1 4 0v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 1 1 2.83 2.83l-.06.06a1.65 1.65 0 0 0-.33 1.82V9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 1 1 0 4h-.09a1.65 1.65 0 0 0-1.51 1z"></path>
      </svg>
    </button>
    <div v-if="open" class="panel">
      <div class="title">Edges</div>
      <label class="row">
        <input
          type="checkbox"
          :checked="showSemantic"
          @change="$emit('update:showSemantic', $event.target.checked)"
        />
        <span class="swatch sem"></span>
        Semantic
      </label>
      <label class="row">
        <input
          type="checkbox"
          :checked="showKeyword"
          @change="$emit('update:showKeyword', $event.target.checked)"
        />
        <span class="swatch kw"></span>
        Keyword
      </label>
      <div class="hint">Supersedes edges always shown.</div>
    </div>
  </div>
</template>

<style scoped>
.settings { position: relative; }
.trigger {
  width: 36px;
  height: 36px;
  display: flex;
  align-items: center;
  justify-content: center;
  background: var(--bg-overlay);
  border: 1px solid var(--border);
  border-radius: var(--radius);
  color: var(--text-dim);
  backdrop-filter: blur(8px);
  box-shadow: var(--shadow);
}
.trigger:hover { color: var(--text); }
.trigger.active { color: var(--accent); border-color: var(--accent); }
.panel {
  position: absolute;
  top: 44px;
  right: 0;
  min-width: 200px;
  padding: 12px;
  background: var(--bg-overlay);
  border: 1px solid var(--border);
  border-radius: var(--radius);
  backdrop-filter: blur(8px);
  box-shadow: var(--shadow);
}
.title {
  font-size: 11px;
  text-transform: uppercase;
  letter-spacing: 0.06em;
  color: var(--text-muted);
  margin-bottom: 6px;
}
.row {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 4px 0;
  cursor: pointer;
  user-select: none;
}
.swatch {
  width: 10px;
  height: 10px;
  border-radius: 2px;
  display: inline-block;
}
.swatch.sem { background: var(--edge-semantic); }
.swatch.kw  { background: var(--edge-keyword); }
.hint {
  margin-top: 8px;
  font-size: 11px;
  color: var(--text-muted);
}
</style>
