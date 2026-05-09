<script setup>
import { ref } from 'vue';

const emit = defineEmits(['search', 'clear']);

const mode = ref('match');
const text = ref('');
const topK = ref(10);
const depth = ref(3);
const beam  = ref(4);

function submit() {
  if (!text.value.trim()) return;
  if (mode.value === 'match')      emit('search', { mode: 'match', text: text.value });
  else if (mode.value === 'search')   emit('search', { mode: 'search', text: text.value, top_k: topK.value });
  else if (mode.value === 'explore')  emit('search', { mode: 'explore', text: text.value, depth: depth.value, beam: beam.value });
}

function clear() {
  text.value = '';
  emit('clear');
}
</script>

<template>
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
      <input v-model.number="topK" type="number" min="1" max="50" class="num" title="top-k" />
    </template>
    <template v-if="mode === 'explore'">
      <input v-model.number="depth" type="number" min="1" max="6" class="num" title="depth" />
      <input v-model.number="beam"  type="number" min="1" max="12" class="num" title="beam" />
    </template>
    <button type="submit" class="primary">Search</button>
    <button type="button" @click="clear" v-if="text">Clear</button>
  </form>
</template>

<style scoped>
.search {
  display: flex;
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
.num  { width: 64px; text-align: center; }
</style>
