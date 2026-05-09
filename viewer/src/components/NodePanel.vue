<script setup>
import { ref, watch, onMounted, onBeforeUnmount, computed } from 'vue';
import { EditorView, keymap, lineNumbers } from '@codemirror/view';
import { EditorState } from '@codemirror/state';
import { defaultKeymap, history, historyKeymap } from '@codemirror/commands';
import { markdown } from '@codemirror/lang-markdown';
import { syntaxHighlighting, defaultHighlightStyle } from '@codemirror/language';
import { api, unwrap } from '../api.js';

const props = defineProps({ nodeId: String });
const emit  = defineEmits(['close', 'saved', 'deleted']);

const loading = ref(false);
const error   = ref('');

const titleRef    = ref('');
const keywordsRef = ref('');
const meta = ref({ author: null, created_at: 0, expires_at: 0, state: 'active' });

let view = null;
let initialTitle = '';
let initialBody = '';
let initialKeywords = '';
const currentBody = ref('');

const editorEl = ref(null);

/* True dirty: actual diff against the loaded values, not just "user typed
 * something". Reverting an edit returns dirty to false naturally. */
const dirty = computed(() => (
  titleRef.value !== initialTitle
  || currentBody.value !== initialBody
  || keywordsRef.value !== initialKeywords
));

/* Resizable panel — width persisted to localStorage between sessions. */
const PANEL_MIN = 360;
const PANEL_MAX_VW_RATIO = 0.7;
const PANEL_DEFAULT = 480;
const panelWidth = ref(PANEL_DEFAULT);
const resizing = ref(false);
function loadWidth() {
  try {
    const v = parseInt(localStorage.getItem('memgraph_panel_width') || '0', 10);
    if (v >= PANEL_MIN) panelWidth.value = v;
  } catch {}
}
function saveWidth(v) {
  try { localStorage.setItem('memgraph_panel_width', String(v)); } catch {}
}
function clampWidth(w) {
  const max = Math.max(PANEL_MIN + 100, Math.floor(window.innerWidth * PANEL_MAX_VW_RATIO));
  return Math.min(max, Math.max(PANEL_MIN, w));
}
function onResizeStart(ev) {
  resizing.value = true;
  ev.preventDefault();
  const startX = ev.clientX;
  const startW = panelWidth.value;
  const move = (e) => {
    const next = clampWidth(startW + (startX - e.clientX));
    panelWidth.value = next;
  };
  const up = () => {
    resizing.value = false;
    window.removeEventListener('pointermove', move);
    window.removeEventListener('pointerup', up);
    saveWidth(panelWidth.value);
  };
  window.addEventListener('pointermove', move);
  window.addEventListener('pointerup', up);
}

function isoUtc(ms) {
  if (!ms) return null;
  try { return new Date(ms).toISOString().replace(/\.\d+Z$/, 'Z'); } catch { return null; }
}

function makeView(initialDoc) {
  const state = EditorState.create({
    doc: initialDoc,
    extensions: [
      lineNumbers(),
      history(),
      keymap.of([...defaultKeymap, ...historyKeymap]),
      markdown(),
      syntaxHighlighting(defaultHighlightStyle),
      EditorView.theme({
        '&':              { height: '100%', backgroundColor: 'var(--bg-elevated)' },
        '.cm-scroller':   { fontFamily: 'var(--mono)', fontSize: '13px' },
        '.cm-content':    { caretColor: 'var(--accent)' },
        '.cm-gutters':    { backgroundColor: 'var(--bg)', color: 'var(--text-muted)', borderRight: '1px solid var(--border-soft)' },
        '.cm-activeLine': { backgroundColor: 'rgba(122,162,247,0.04)' },
        '.cm-selectionBackground': { backgroundColor: 'rgba(122,162,247,0.18) !important' },
      }, { dark: true }),
      EditorView.updateListener.of((u) => {
        if (u.docChanged) currentBody.value = u.state.doc.toString();
      }),
    ],
  });
  return new EditorView({ state, parent: editorEl.value });
}

async function load() {
  loading.value = true;
  error.value = '';
  try {
    const res = unwrap(await api.get(props.nodeId));
    initialTitle    = res.title || '';
    initialBody     = res.body  || '';
    initialKeywords = (res.keywords || []).join(', ');
    titleRef.value    = initialTitle;
    keywordsRef.value = initialKeywords;
    currentBody.value = initialBody;
    meta.value = {
      author: res.author || null,
      created_at: res.created_at || 0,
      expires_at: res.expires_at || 0,
      state: res.state || 'active',
    };
    if (view) { view.destroy(); view = null; }
    if (editorEl.value) view = makeView(initialBody);
  } catch (e) {
    error.value = e.message || String(e);
  } finally {
    loading.value = false;
  }
}

async function save() {
  if (!dirty.value) return;
  loading.value = true;
  error.value = '';
  try {
    const body = view ? view.state.doc.toString() : initialBody;
    const keywords = keywordsRef.value
      .split(',')
      .map((k) => k.trim())
      .filter(Boolean);
    const payload = {
      title: titleRef.value,
      body,
      keywords,
      supersedes: props.nodeId,
    };
    const res = unwrap(await api.insert(payload));
    emit('saved', res?.id_hex || null);
  } catch (e) {
    error.value = e.message || String(e);
  } finally {
    loading.value = false;
  }
}

async function remove() {
  if (!confirm('Delete this node? Edges to/from it will cascade.')) return;
  loading.value = true;
  error.value = '';
  try {
    await api.remove(props.nodeId);
    emit('deleted', props.nodeId);
  } catch (e) {
    error.value = e.message || String(e);
  } finally {
    loading.value = false;
  }
}

watch(() => props.nodeId, (v) => { if (v) load(); }, { immediate: true });

onMounted(loadWidth);
onBeforeUnmount(() => { if (view) { view.destroy(); view = null; } });

const dateStr   = computed(() => isoUtc(meta.value.created_at));
const expiryStr = computed(() => isoUtc(meta.value.expires_at));
</script>

<template>
  <aside class="panel" :style="{ width: panelWidth + 'px' }" :class="{ resizing }">
    <div class="resize-handle" @pointerdown="onResizeStart" title="Drag to resize" />

    <header class="head">
      <div class="state-pill" :class="meta.state">{{ meta.state }}</div>
      <div class="spacer" />
      <button class="ghost" @click="$emit('close')" title="Close">×</button>
    </header>

    <div v-if="error" class="error">{{ error }}</div>

    <label class="lbl">Title</label>
    <input v-model="titleRef" class="title" />

    <label class="lbl">Body (Markdown)</label>
    <div class="editor-wrap">
      <div ref="editorEl" class="editor" />
    </div>

    <label class="lbl">Keywords <span class="hint">comma-separated</span></label>
    <input v-model="keywordsRef" class="keywords" />

    <dl class="meta">
      <template v-if="meta.author">
        <dt>Author</dt><dd>{{ meta.author }}</dd>
      </template>
      <template v-if="dateStr">
        <dt>Created</dt><dd>{{ dateStr }}</dd>
      </template>
      <template v-if="expiryStr">
        <dt>Expires</dt><dd>{{ expiryStr }}</dd>
      </template>
      <dt>id_hex</dt><dd class="mono">{{ nodeId }}</dd>
    </dl>

    <footer class="actions">
      <button class="danger" :disabled="loading" @click="remove">Delete</button>
      <div class="spacer" />
      <button :disabled="loading" @click="$emit('close')">Cancel</button>
      <button class="primary" :disabled="loading || !dirty" @click="save">
        {{ dirty ? 'Save (supersedes)' : 'Saved' }}
      </button>
    </footer>
  </aside>
</template>

<style scoped>
.panel {
  position: relative;
  height: 100%;
  display: flex;
  flex-direction: column;
  background: var(--bg-elevated);
  border-left: 1px solid var(--border-soft);
  padding: 14px 16px 14px 18px;
  gap: 8px;
  transition: width 0.18s ease;
  box-shadow: -8px 0 24px rgba(0, 0, 0, 0.25);
}
.panel.resizing { transition: none; user-select: none; }

.resize-handle {
  position: absolute;
  top: 0;
  left: 0;
  width: 6px;
  height: 100%;
  cursor: ew-resize;
  z-index: 5;
}
.resize-handle::before {
  content: '';
  position: absolute;
  inset: 0;
  background: transparent;
  border-left: 1px solid var(--border-soft);
  transition: border-color 0.15s ease;
}
.resize-handle:hover::before,
.panel.resizing .resize-handle::before {
  border-left-color: var(--accent);
}
.head { display: flex; align-items: center; }
.spacer { flex: 1; }
.ghost {
  border: none;
  background: transparent;
  color: var(--text-dim);
  font-size: 22px;
  line-height: 1;
  padding: 4px 8px;
}
.ghost:hover { color: var(--text); }

.state-pill {
  font-size: 10px;
  text-transform: uppercase;
  letter-spacing: 0.08em;
  padding: 3px 8px;
  border-radius: 999px;
  background: var(--accent-soft);
  color: var(--accent);
  border: 1px solid var(--accent);
}
.state-pill.superseded {
  background: rgba(91,100,120,0.15);
  border-color: var(--node-superseded);
  color: var(--node-superseded);
}

.error {
  background: rgba(248,113,113,0.08);
  border: 1px solid rgba(248,113,113,0.5);
  color: #f87171;
  padding: 8px 10px;
  border-radius: var(--radius-sm);
  font-size: 12px;
}

.lbl {
  font-size: 11px;
  text-transform: uppercase;
  letter-spacing: 0.06em;
  color: var(--text-muted);
  margin-top: 6px;
}
.lbl .hint {
  text-transform: none;
  letter-spacing: 0;
  color: var(--text-muted);
  margin-left: 6px;
  font-size: 11px;
}

.title { font-size: 14px; }
.keywords { font-family: var(--mono); font-size: 12px; }

.editor-wrap {
  flex: 1 1 auto;
  min-height: 200px;
  border: 1px solid var(--border);
  border-radius: var(--radius-sm);
  overflow: hidden;
  background: var(--bg-elevated);
}
.editor { height: 100%; }
.editor :deep(.cm-editor) { height: 100%; }
.editor :deep(.cm-editor.cm-focused) { outline: none; }

.meta {
  margin: 4px 0 0 0;
  display: grid;
  grid-template-columns: max-content 1fr;
  gap: 4px 12px;
  font-size: 12px;
  color: var(--text-dim);
}
.meta dt { color: var(--text-muted); }
.meta dd { margin: 0; }
.mono { font-family: var(--mono); font-size: 11px; word-break: break-all; }

.actions {
  display: flex;
  gap: 8px;
  margin-top: 12px;
}
</style>
