<script setup>
import { ref, computed, watch } from 'vue';

const props = defineProps({
  /* All distinct keyword strings present in the graph — feeds the
   * autocomplete dropdown in Explore mode. */
  allKeywords: { type: Array, default: () => [] },
});
const emit = defineEmits(['search', 'clear']);

const mode  = ref('match');
const text  = ref('');
const topK  = ref(10);
const depth = ref(5);
const beam  = ref(1);

/* Explore-only keyword chips + autocomplete */
const kwInput = ref('');
const kwChips = ref([]);
const kwHi    = ref(-1);  // highlighted suggestion index for keyboard nav
const AUTOCOMPLETE_MIN = 3;

const kwSuggestions = computed(() => {
  const q = kwInput.value.trim().toLowerCase();
  if (q.length < AUTOCOMPLETE_MIN) return [];
  const set = new Set(kwChips.value);
  return props.allKeywords
    .filter((k) => k.toLowerCase().includes(q) && !set.has(k))
    .slice(0, 8);
});

/* When the user is typing 3+ chars but nothing matches, surface that
 * explicitly instead of just hiding the dropdown — saves them from
 * wondering "is autocomplete broken?" */
const kwShowEmpty = computed(() => {
  const q = kwInput.value.trim();
  return q.length >= AUTOCOMPLETE_MIN && kwSuggestions.value.length === 0;
});

const kwPlaceholder = computed(() => {
  const total = props.allKeywords.length;
  if (!total) return 'Loading keywords from graph…';
  return `Filter by keyword — ${total} available, type ${AUTOCOMPLETE_MIN}+ letters`;
});

watch(kwSuggestions, () => { kwHi.value = -1; });

function addChip(kw) {
  const k = kw.trim();
  if (!k) return;
  if (kwChips.value.includes(k)) return;
  kwChips.value.push(k);
  kwInput.value = '';
  kwHi.value = -1;
}

function removeChip(kw) {
  kwChips.value = kwChips.value.filter((k) => k !== kw);
}

function onKwKey(ev) {
  const list = kwSuggestions.value;
  if (ev.key === 'ArrowDown' && list.length) {
    kwHi.value = (kwHi.value + 1) % list.length;
    ev.preventDefault();
  } else if (ev.key === 'ArrowUp' && list.length) {
    kwHi.value = (kwHi.value - 1 + list.length) % list.length;
    ev.preventDefault();
  } else if (ev.key === 'Enter' && list.length) {
    addChip(list[Math.max(0, kwHi.value)] ?? list[0]);
    ev.preventDefault();
    ev.stopPropagation();   // don't submit the form
  } else if (ev.key === 'Backspace' && !kwInput.value && kwChips.value.length) {
    removeChip(kwChips.value[kwChips.value.length - 1]);
  }
}

function submit() {
  if (!text.value.trim()) return;
  if (mode.value === 'match') {
    emit('search', { mode: 'match', text: text.value });
  } else if (mode.value === 'search') {
    emit('search', { mode: 'search', text: text.value, top_k: topK.value });
  } else if (mode.value === 'explore') {
    emit('search', {
      mode: 'explore',
      text: text.value,
      depth: depth.value,
      beam: beam.value,
      keywords: kwChips.value.slice(),
    });
  }
}

function clear() {
  text.value = '';
  kwInput.value = '';
  kwChips.value = [];
  kwHi.value = -1;
  emit('clear');
}
</script>

<template>
  <div class="search-shell">
    <form class="search" @submit.prevent="submit">
      <select v-model="mode" class="mode" :title="'Search mode'">
        <option value="match">Match</option>
        <option value="search">Retrieve</option>
        <option value="explore">Explore</option>
      </select>
      <input
        v-model="text"
        class="text"
        :placeholder="
          mode === 'match'   ? 'Cache lookup — exact match' :
          mode === 'search'  ? 'Hybrid top-k retrieval' :
                                'Graph walk from a seed'
        "
        autocomplete="off"
      />
      <template v-if="mode === 'search'">
        <label class="num-field">
          <span>top-k</span>
          <input v-model.number="topK" type="number" min="1" max="50" class="num" />
        </label>
      </template>
      <template v-if="mode === 'explore'">
        <label class="num-field">
          <span>depth</span>
          <input v-model.number="depth" type="number" min="1" max="6" class="num" />
        </label>
        <label class="num-field">
          <span>beam</span>
          <input v-model.number="beam" type="number" min="1" max="12" class="num" />
        </label>
      </template>
      <button type="submit" class="primary">Search</button>
      <button type="button" @click="clear" v-if="text || kwChips.length">Clear</button>
    </form>

    <div v-if="mode === 'explore'" class="kw-row">
      <div class="kw-input-wrap">
        <input
          v-model="kwInput"
          class="kw-input"
          :placeholder="kwPlaceholder"
          autocomplete="off"
          @keydown="onKwKey"
        />
        <ul v-if="kwSuggestions.length" class="kw-suggest">
          <li
            v-for="(s, i) in kwSuggestions"
            :key="s"
            :class="{ hi: i === kwHi }"
            @mousedown.prevent="addChip(s)"
            @mouseenter="kwHi = i"
          >#{{ s }}</li>
        </ul>
        <div v-else-if="kwShowEmpty" class="kw-suggest empty">no keywords match "{{ kwInput }}"</div>
      </div>
      <div v-if="kwChips.length" class="chips">
        <span v-for="kw in kwChips" :key="kw" class="chip">
          #{{ kw }}
          <button type="button" class="chip-x" @click="removeChip(kw)">×</button>
        </span>
      </div>
    </div>
  </div>
</template>

<style scoped>
.search-shell { display: flex; flex-direction: column; gap: 6px; }
.search {
  display: flex;
  align-items: center;
  gap: 6px;
  background: var(--bg-overlay);
  backdrop-filter: blur(8px);
  border: 1px solid var(--border);
  border-radius: var(--radius);
  padding: 6px;
  box-shadow: var(--shadow);
}
.mode { width: 110px; }
.text { flex: 1; min-width: 280px; }

.num-field {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  background: var(--bg);
  border: 1px solid var(--border);
  border-radius: var(--radius-sm);
  padding: 0 0 0 10px;
  height: 30px;
  color: var(--text-muted);
  font-size: 11px;
  text-transform: uppercase;
  letter-spacing: 0.04em;
}
.num-field input.num {
  width: 50px;
  height: 30px;
  border: none;
  border-left: 1px solid var(--border);
  background: transparent;
  text-align: center;
  font-family: var(--mono);
  color: var(--text);
  text-transform: none;
  letter-spacing: 0;
  font-size: 13px;
  margin-left: 4px;
  padding: 0;
}
.num-field input.num:focus { box-shadow: none; }

.kw-row {
  display: flex;
  align-items: flex-start;
  flex-wrap: wrap;
  gap: 8px;
  background: var(--bg-overlay);
  border: 1px solid var(--border);
  border-radius: var(--radius);
  padding: 6px 8px;
  backdrop-filter: blur(8px);
  box-shadow: var(--shadow);
}
.kw-input-wrap {
  position: relative;
  flex: 1;
  min-width: 240px;
}
.kw-input {
  width: 100%;
  background: transparent;
  border: none;
  padding: 4px 6px;
  font-size: 12.5px;
}
.kw-input:focus { box-shadow: none; }
.kw-suggest {
  position: absolute;
  top: 100%;
  left: 0;
  right: 0;
  margin: 4px 0 0 0;
  padding: 4px 0;
  list-style: none;
  background: rgba(34, 38, 50, 0.96);
  border: 1px solid rgba(122, 162, 247, 0.32);
  border-radius: var(--radius-sm);
  box-shadow: 0 8px 24px rgba(0, 0, 0, 0.5);
  max-height: 240px;
  overflow-y: auto;
  z-index: 20;
  font-family: var(--mono);
  font-size: 12px;
}
.kw-suggest li {
  padding: 5px 12px;
  color: var(--text);
  cursor: pointer;
}
.kw-suggest li.hi,
.kw-suggest li:hover {
  background: rgba(122, 162, 247, 0.14);
  color: var(--accent);
}
.kw-suggest.empty {
  padding: 8px 12px;
  color: var(--text-muted);
  font-style: italic;
}

.chips {
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
  align-items: center;
}
.chip {
  display: inline-flex;
  align-items: center;
  gap: 4px;
  font-family: var(--mono);
  font-size: 11.5px;
  padding: 2px 4px 2px 8px;
  background: rgba(125, 211, 252, 0.12);
  color: var(--edge-keyword);
  border: 1px solid rgba(125, 211, 252, 0.45);
  border-radius: 999px;
  line-height: 1.4;
}
.chip-x {
  background: transparent;
  border: none;
  color: var(--edge-keyword);
  font-size: 14px;
  line-height: 1;
  padding: 0 4px;
  cursor: pointer;
  border-radius: 999px;
}
.chip-x:hover { background: rgba(125, 211, 252, 0.22); color: var(--text); }
</style>
