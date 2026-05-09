<script setup>
import { ref, watch, onMounted, onBeforeUnmount, shallowRef } from 'vue';
import * as THREE from 'three';
import { OrbitControls } from 'three/addons/controls/OrbitControls.js';
import { CSS2DRenderer, CSS2DObject } from 'three/addons/renderers/CSS2DRenderer.js';
import { LineSegmentsGeometry } from 'three/addons/lines/LineSegmentsGeometry.js';
import { LineSegments2 } from 'three/addons/lines/LineSegments2.js';
import { LineMaterial } from 'three/addons/lines/LineMaterial.js';

const props = defineProps({
  nodes: { type: Array, default: () => [] },
  edges: { type: Array, default: () => [] },
  selectedId: { type: String, default: null },
  highlightedIds: { type: Array, default: () => [] },
  showSemantic: { type: Boolean, default: true },
  showKeyword:  { type: Boolean, default: true },
  dimNonHighlighted: { type: Boolean, default: false },
  colorRamp:    { type: String, default: null },  // null | 'orange'
});
const emit = defineEmits(['select', 'select-edge']);

const container = ref(null);

let renderer, labelRenderer, scene, camera, controls, raycaster, mouse;
let nodeGroup, edgeGroup;
const nodeMeshes = shallowRef(new Map());     // id_hex -> Mesh
const nodeLabels = shallowRef(new Map());     // id_hex -> CSS2DObject
let edgeLineObjects = [];                     // { obj, perSegment: edge[] }
let animationFrame;
let hoveredId = null;
let lastHoverCheck = 0;
const camTarget = { pos: new THREE.Vector3(), look: new THREE.Vector3() };
let camAnimating = false;
const clock = new THREE.Clock();

/* Tunables — coords are scaled, then nodes sized smaller relative to spread,
 * so edges read as actual lines instead of overlapping dots. */
const SPATIAL_SCALE     = 210.0; // multiplier on projected coords — wide-open scene
const NODE_R_BASE       = 0.12;  // sphere radius for an "average" body
const NODE_R_MIN        = 0.08;
const NODE_R_MAX        = 0.30;
const NODE_R_LOG_FACTOR = 0.07;  // delta per ln(body_len)
const LABEL_NEAR_DIST   = 8.0;   // close-zoom only (user-tuned)
/* Edge thickness scales with weight. We bucket by quartile of the visible
 * weight range (per kind) so we still get just a few LineSegments2 objects
 * per kind instead of one per edge. */
const EDGE_LW_MIN       = 0.4;
const EDGE_LW_MAX       = 1.8;
const EDGE_LW_BUCKETS   = 4;

const COLOR = {
  bg:             0x0e1014,
  superseded:     0x5b6478,
  selected:       0x7aa2f7,
  highlighted:    0xa6e3a1,
  edgeSemantic:   0xbef264,
  edgeKeyword:    0x7dd3fc,
  edgeSupersedes: 0xf87171,
};

/* Deterministic hue from a string (FNV-1a 32-bit). Returns a THREE.Color
 * with elegant fixed saturation/lightness for dark mode. */
function colorFromString(s) {
  let h = 0x811c9dc5 >>> 0;
  for (let i = 0; i < (s || '').length; i++) {
    h ^= s.charCodeAt(i);
    h = Math.imul(h, 0x01000193) >>> 0;
  }
  const hue = (h % 360) / 360;
  return new THREE.Color().setHSL(hue, 0.42, 0.62);
}

function nodeColor(n) {
  if (n.state === 'superseded') return new THREE.Color(COLOR.superseded);
  return colorFromString(n.primary_keyword || n.title || n.id_hex);
}

function nodeRadius(n) {
  const len = n.body_len ?? 0;
  if (len <= 0) return NODE_R_MIN;
  const r = NODE_R_BASE + Math.log(1 + len) * NODE_R_LOG_FACTOR;
  return Math.min(NODE_R_MAX, Math.max(NODE_R_MIN, r));
}

function disposeGroup(g) {
  if (!g) return;
  while (g.children.length) {
    const m = g.children.pop();
    m.geometry?.dispose?.();
    if (Array.isArray(m.material)) m.material.forEach((x) => x.dispose());
    else m.material?.dispose?.();
  }
}

function makeLabel(text) {
  const div = document.createElement('div');
  div.className = 'g3d-label';
  div.textContent = text || '';
  return new CSS2DObject(div);
}

function rebuildNodes() {
  // Tear down old labels (parented to old meshes)
  for (const [, label] of nodeLabels.value) label.element.remove();
  nodeLabels.value.clear();

  disposeGroup(nodeGroup);
  nodeMeshes.value.clear();

  for (const n of props.nodes) {
    const r = nodeRadius(n);
    const geom = new THREE.SphereGeometry(r, 28, 20);
    /* MeshLambertMaterial gives soft Lambertian shading — spheres look like
     * spheres, not flat discs, with very low GPU cost. Subtle emissive lifts
     * the unlit side enough to read against the dark background. */
    const baseColor = nodeColor(n);
    const mat = new THREE.MeshLambertMaterial({
      color: baseColor,
      emissive: baseColor.clone().multiplyScalar(0.18),
    });
    const m = new THREE.Mesh(geom, mat);
    m.position.set(n.x * SPATIAL_SCALE, n.y * SPATIAL_SCALE, n.z * SPATIAL_SCALE);
    m.userData = { id: n.id_hex, title: n.title, state: n.state, baseColor: mat.color.clone(), radius: r };
    nodeGroup.add(m);
    nodeMeshes.value.set(n.id_hex, m);

    const label = makeLabel(n.title);
    // Anchor label slightly above the sphere
    label.position.set(0, r + 0.02, 0);
    m.add(label);
    nodeLabels.value.set(n.id_hex, label);
    label.visible = false;
  }
}

/* Pick which edges of `kind` to render. By default we keep only the top-N
 * outgoing per source node — this drops the visual clutter dramatically
 * without losing the dominant relationships. When a search highlight is
 * active, we also keep edges that connect two highlighted nodes, so the
 * relevant subgraph is fully visible even if those edges aren't top-N.
 *
 * Applied independently to semantic and keyword kinds. Supersedes is
 * never filtered — it's rare and load-bearing for the history view. */
const TOP_PER_SRC = 2;
function chooseTopEdges(allEdges, kind, highlightedSet) {
  const bySrc = new Map();
  for (const e of allEdges) {
    if (e.kind !== kind) continue;
    if (!bySrc.has(e.src)) bySrc.set(e.src, []);
    bySrc.get(e.src).push(e);
  }
  const seen = new Set();
  const out = [];
  const keyOf = (e) => `${e.src}|${e.dst}|${e.kind}`;
  for (const list of bySrc.values()) {
    list.sort((a, b) => (b.weight ?? 0) - (a.weight ?? 0));
    for (const e of list.slice(0, TOP_PER_SRC)) {
      const k = keyOf(e);
      if (!seen.has(k)) { seen.add(k); out.push(e); }
    }
  }
  if (highlightedSet && highlightedSet.size > 1) {
    for (const e of allEdges) {
      if (e.kind !== kind) continue;
      if (!highlightedSet.has(e.src) || !highlightedSet.has(e.dst)) continue;
      const k = keyOf(e);
      if (!seen.has(k)) { seen.add(k); out.push(e); }
    }
  }
  return out;
}

function rebuildEdges() {
  // Dispose previous line objects
  for (const e of edgeLineObjects) {
    edgeGroup.remove(e.obj);
    e.obj.geometry?.dispose?.();
    e.obj.material?.dispose?.();
  }
  edgeLineObjects = [];

  const highlightedSet = new Set(props.highlightedIds || []);
  const keyOf = (e) => `${e.src}|${e.dst}|${e.kind}`;

  /* Edge-visibility strategies:
   *  - DEFAULT: top-2 outgoing per source for semantic + keyword, plus any
   *    edge connecting two highlighted nodes (search-aware top-up).
   *  - DIM MODE: edges restricted to the highlighted subgraph. Semantic edges
   *    are still capped at top-2-per-source within that subgraph (avoids
   *    clutter when a node has many strong semantic links to results).
   *    Keyword and supersedes edges show every member of the subgraph. */
  const semanticVisible = new Set(chooseTopEdges(props.edges, 'semantic', highlightedSet).map(keyOf));
  const keywordVisible  = new Set(chooseTopEdges(props.edges, 'keyword',  highlightedSet).map(keyOf));
  /* Top-2 semantic restricted to the subgraph — used in dim mode. */
  const subgraphEdges = props.dimNonHighlighted
    ? props.edges.filter((e) => highlightedSet.has(e.src) && highlightedSet.has(e.dst))
    : [];
  const semanticSubgraphTop2 = new Set(
    chooseTopEdges(subgraphEdges, 'semantic', null).map(keyOf)
  );

  /* Group visible edges by kind */
  const visiblePerKind = { semantic: [], keyword: [], supersedes: [] };
  for (const e of props.edges) {
    const a = nodeMeshes.value.get(e.src);
    const b = nodeMeshes.value.get(e.dst);
    if (!a || !b) continue;

    if (props.dimNonHighlighted) {
      if (!highlightedSet.has(e.src) || !highlightedSet.has(e.dst)) continue;
      if (e.kind === 'semantic') {
        if (!props.showSemantic) continue;
        if (!semanticSubgraphTop2.has(keyOf(e))) continue;
      } else if (e.kind === 'keyword') {
        if (!props.showKeyword) continue;
      } else if (e.kind !== 'supersedes') {
        continue;
      }
    } else if (e.kind === 'semantic') {
      if (!props.showSemantic) continue;
      if (!semanticVisible.has(keyOf(e))) continue;
    } else if (e.kind === 'keyword') {
      if (!props.showKeyword) continue;
      if (!keywordVisible.has(keyOf(e))) continue;
    } else if (e.kind !== 'supersedes') {
      continue;
    }
    visiblePerKind[e.kind].push(e);
  }

  const KIND_STYLE = {
    semantic:   { color: COLOR.edgeSemantic,   opacity: 0.7  },
    keyword:    { color: COLOR.edgeKeyword,    opacity: 0.55 },
    supersedes: { color: COLOR.edgeSupersedes, opacity: 0.85 },
  };

  const w = renderer.domElement.clientWidth;
  const h = renderer.domElement.clientHeight;

  /* For each kind: bucket edges by weight quartile, build one LineSegments2
   * per non-empty bucket with linewidth proportional to the bucket index.
   * Result: edges with higher weight render visibly thicker. */
  for (const kind of Object.keys(visiblePerKind)) {
    const list = visiblePerKind[kind];
    if (!list.length) continue;
    const style = KIND_STYLE[kind];

    let wMin = Infinity, wMax = -Infinity;
    for (const e of list) {
      const ww = e.weight ?? 0;
      if (ww < wMin) wMin = ww;
      if (ww > wMax) wMax = ww;
    }
    const span = wMax - wMin;

    const buckets = Array.from({ length: EDGE_LW_BUCKETS }, () => ({ positions: [], edges: [] }));
    for (const e of list) {
      const t = span > 1e-6 ? ((e.weight ?? 0) - wMin) / span : 0.5;
      const bi = Math.min(EDGE_LW_BUCKETS - 1, Math.floor(t * EDGE_LW_BUCKETS));
      const a = nodeMeshes.value.get(e.src);
      const b = nodeMeshes.value.get(e.dst);
      buckets[bi].positions.push(
        a.position.x, a.position.y, a.position.z,
        b.position.x, b.position.y, b.position.z,
      );
      buckets[bi].edges.push(e);
    }

    for (let i = 0; i < EDGE_LW_BUCKETS; ++i) {
      const b = buckets[i];
      if (!b.positions.length) continue;
      const lw = EDGE_LW_MIN + (i / Math.max(1, EDGE_LW_BUCKETS - 1)) * (EDGE_LW_MAX - EDGE_LW_MIN);
      const geom = new LineSegmentsGeometry();
      geom.setPositions(b.positions);
      const mat = new LineMaterial({
        color: style.color,
        linewidth: lw,
        transparent: true,
        opacity: style.opacity,
        worldUnits: false,
        dashed: false,
      });
      mat.resolution.set(w, h);
      const lines = new LineSegments2(geom, mat);
      lines.computeLineDistances();
      lines.userData = { kind, segments: b.edges };
      edgeGroup.add(lines);
      edgeLineObjects.push({ obj: lines, perSegment: b.edges });
    }
  }
}

/* Rank ramp: deep red → light orange, sharp tier-stepped. Discrete buckets
 * give a more decisive visual stratification than a smooth gradient. */
const RANK_COLORS = [
  '#b30000', // 1: vivid dark red — most relevant
  '#e63b00', // 2: punchy red-orange
  '#ff7f00', // 3: full orange
  '#ffb84d', // 4: bright light orange
  '#ffe0b3', // 5: pale peach — least relevant
];
function rankColorOrange(idx, total) {
  if (total <= 0) return new THREE.Color(RANK_COLORS[0]);
  const denom = Math.max(1, total - 1);
  const t = idx / denom;
  const bucket = Math.min(RANK_COLORS.length - 1, Math.floor(t * RANK_COLORS.length));
  return new THREE.Color(RANK_COLORS[bucket]);
}

function applyHighlights() {
  const sel = props.selectedId;
  const list = props.highlightedIds || [];
  const idxOf = new Map();
  list.forEach((id, i) => idxOf.set(id, i));
  const isRanked = props.colorRamp === 'orange';
  const dim = props.dimNonHighlighted;

  for (const [id, mesh] of nodeMeshes.value) {
    let color;
    const inSet = idxOf.has(id);
    let opacity = 1.0;

    if (id === sel) {
      color = new THREE.Color(COLOR.selected);
    } else if (inSet) {
      color = isRanked
        ? rankColorOrange(idxOf.get(id), list.length)
        : new THREE.Color(COLOR.highlighted);
    } else {
      color = mesh.userData.baseColor;
      if (dim) opacity = 0.12;
    }

    mesh.userData.targetScale = (id === sel) ? 1.55 : (inSet ? 1.30 : 1.0);
    mesh.material.color.copy(color);
    /* Dim non-highlighted via material opacity. emissive is left as is so
     * the dimmed nodes still read as 3D, just much darker. */
    mesh.material.transparent = opacity < 1.0;
    mesh.material.opacity = opacity;
    mesh.material.depthWrite = opacity >= 1.0;
    mesh.material.needsUpdate = true;
  }
}

/* Each frame: lerp current scale toward target, add hover/selected pulse. */
function animateNodes(t) {
  for (const [id, mesh] of nodeMeshes.value) {
    const target = mesh.userData.targetScale ?? 1.0;
    const hover = (id === hoveredId) ? 0.18 : 0.0;
    const pulse = (id === props.selectedId) ? Math.sin(t * 3.0) * 0.06 : 0.0;
    const desired = target + hover + pulse;
    const cur = mesh.scale.x;
    const next = cur + (desired - cur) * 0.18;  // ease toward desired
    mesh.scale.setScalar(next);
  }
}

/* Camera smooth-focus when selection changes — keeps the orbit framing
 * but slides the target into view over a few frames. */
function updateCameraAnim() {
  if (!camAnimating) return;
  controls.target.lerp(camTarget.look, 0.10);
  camera.position.lerp(camTarget.pos, 0.10);
  if (controls.target.distanceToSquared(camTarget.look) < 1e-5
      && camera.position.distanceToSquared(camTarget.pos) < 1e-5) {
    controls.target.copy(camTarget.look);
    camera.position.copy(camTarget.pos);
    camAnimating = false;
  }
}

function focusOnSelected() {
  const sel = props.selectedId;
  if (!sel) return;
  const mesh = nodeMeshes.value.get(sel);
  if (!mesh) return;
  const dir = camera.position.clone().sub(controls.target).normalize();
  const dist = camera.position.distanceTo(controls.target);
  const targetDist = Math.max(1.5, Math.min(dist, 6.0));
  camTarget.look.copy(mesh.position);
  camTarget.pos.copy(mesh.position).add(dir.multiplyScalar(targetDist));
  camAnimating = true;
}

/* Cheap hover detection (every ~50ms, not every frame). */
function pollHover(now) {
  if (now - lastHoverCheck < 50) return;
  lastHoverCheck = now;
  raycaster.setFromCamera(mouse, camera);
  const nodeHits = raycaster.intersectObjects(nodeGroup.children, false);
  let nextHover = null;
  if (nodeHits.length) nextHover = nodeHits[0].object.userData.id;
  if (nextHover !== hoveredId) {
    hoveredId = nextHover;
    if (renderer?.domElement) {
      renderer.domElement.style.cursor = hoveredId ? 'pointer' : 'grab';
    }
  }
}

function updateLabelVisibility() {
  if (!camera) return;
  for (const [id, label] of nodeLabels.value) {
    const mesh = nodeMeshes.value.get(id);
    if (!mesh) continue;
    const dist = camera.position.distanceTo(mesh.position);
    label.visible = (dist < LABEL_NEAR_DIST) || (id === props.selectedId);
  }
}

function frameToFit() {
  if (!props.nodes.length) return;
  const box = new THREE.Box3();
  for (const n of props.nodes) {
    box.expandByPoint(new THREE.Vector3(
      n.x * SPATIAL_SCALE, n.y * SPATIAL_SCALE, n.z * SPATIAL_SCALE
    ));
  }
  const size = new THREE.Vector3();
  box.getSize(size);
  const center = new THREE.Vector3();
  box.getCenter(center);
  const radius = Math.max(size.x, size.y, size.z, 0.5);
  camera.position.copy(center).add(new THREE.Vector3(radius * 1.4, radius * 1.0, radius * 1.8));
  controls.target.copy(center);
  controls.update();
}

function setMouseFromEvent(ev) {
  const rect = renderer.domElement.getBoundingClientRect();
  mouse.x = ((ev.clientX - rect.left) / rect.width) * 2 - 1;
  mouse.y = -((ev.clientY - rect.top) / rect.height) * 2 + 1;
}

function pickNode() {
  const hits = raycaster.intersectObjects(nodeGroup.children, false);
  return hits[0] || null;
}

function pickEdge() {
  // LineSegments2 raycast yields { faceIndex } where each segment is one face.
  raycaster.params.Line2 = { threshold: 6 };  // pixels — generous click target
  for (const lo of edgeLineObjects) {
    const hits = raycaster.intersectObject(lo.obj, false);
    if (hits.length) {
      const idx = hits[0].faceIndex ?? 0;
      const edge = lo.perSegment[idx];
      if (edge) return { hit: hits[0], edge, midpoint: hits[0].point.clone() };
    }
  }
  return null;
}

function onClick(ev) {
  setMouseFromEvent(ev);
  raycaster.setFromCamera(mouse, camera);
  const nodeHit = pickNode();
  if (nodeHit) {
    emit('select', nodeHit.object.userData.id);
    return;
  }
  const edgeHit = pickEdge();
  if (edgeHit) {
    emit('select-edge', { ...edgeHit.edge, screenX: ev.clientX, screenY: ev.clientY });
  }
}

function onMouseMove(ev) {
  setMouseFromEvent(ev);
  // Hover is polled in the render loop using current mouse position.
}

function onResize() {
  const w = container.value.clientWidth;
  const h = container.value.clientHeight;
  renderer.setSize(w, h, false);
  labelRenderer.setSize(w, h);
  camera.aspect = w / h;
  camera.updateProjectionMatrix();
  for (const lo of edgeLineObjects) {
    lo.obj.material.resolution.set(w, h);
  }
}

function loop() {
  const t = clock.getElapsedTime();
  const now = performance.now();
  pollHover(now);
  animateNodes(t);
  updateCameraAnim();
  controls.update();
  updateLabelVisibility();
  renderer.render(scene, camera);
  labelRenderer.render(scene, camera);
  animationFrame = requestAnimationFrame(loop);
}

onMounted(() => {
  scene = new THREE.Scene();
  scene.background = new THREE.Color(COLOR.bg);

  const w = container.value.clientWidth;
  const h = container.value.clientHeight;
  camera = new THREE.PerspectiveCamera(55, w / h, 0.001, 1000);
  camera.position.set(8, 6, 10);

  renderer = new THREE.WebGLRenderer({ antialias: true });
  renderer.setPixelRatio(window.devicePixelRatio);
  renderer.setSize(w, h);
  container.value.appendChild(renderer.domElement);

  labelRenderer = new CSS2DRenderer();
  labelRenderer.setSize(w, h);
  labelRenderer.domElement.style.position = 'absolute';
  labelRenderer.domElement.style.top = '0';
  labelRenderer.domElement.style.left = '0';
  labelRenderer.domElement.style.pointerEvents = 'none';
  container.value.appendChild(labelRenderer.domElement);

  controls = new OrbitControls(camera, renderer.domElement);
  controls.enableDamping = true;
  controls.dampingFactor = 0.12;

  /* Lighting — soft global + a single warm directional for shape. Very
   * cheap on CPU/GPU, dramatic improvement over flat shading. */
  scene.add(new THREE.AmbientLight(0xffffff, 0.55));
  const key = new THREE.DirectionalLight(0xffffff, 0.85);
  key.position.set(4, 6, 8);
  scene.add(key);
  const fill = new THREE.DirectionalLight(0x7aa2f7, 0.25);
  fill.position.set(-6, -2, -4);
  scene.add(fill);

  nodeGroup = new THREE.Group();
  edgeGroup = new THREE.Group();
  scene.add(edgeGroup);
  scene.add(nodeGroup);

  raycaster = new THREE.Raycaster();
  raycaster.params.Line2 = { threshold: 6 };
  mouse = new THREE.Vector2();

  renderer.domElement.addEventListener('click', onClick);
  renderer.domElement.addEventListener('mousemove', onMouseMove);
  window.addEventListener('resize', onResize);

  rebuildNodes();
  rebuildEdges();
  applyHighlights();
  frameToFit();
  loop();
});

onBeforeUnmount(() => {
  cancelAnimationFrame(animationFrame);
  window.removeEventListener('resize', onResize);
  renderer?.domElement.removeEventListener('click', onClick);
  renderer?.domElement.removeEventListener('mousemove', onMouseMove);
  for (const [, label] of nodeLabels.value) label.element.remove();
  nodeLabels.value.clear();
  disposeGroup(nodeGroup);
  for (const lo of edgeLineObjects) {
    lo.obj.geometry?.dispose?.();
    lo.obj.material?.dispose?.();
  }
  edgeLineObjects = [];
  controls?.dispose();
  renderer?.dispose();
  if (labelRenderer?.domElement?.parentNode) {
    labelRenderer.domElement.parentNode.removeChild(labelRenderer.domElement);
  }
});

watch(() => props.nodes,    () => { rebuildNodes(); rebuildEdges(); applyHighlights(); frameToFit(); });
watch(() => props.edges,    () => rebuildEdges());
watch(() => props.showSemantic, () => rebuildEdges());
watch(() => props.showKeyword,  () => rebuildEdges());
watch(() => props.selectedId, () => { applyHighlights(); focusOnSelected(); });
watch(() => props.highlightedIds, () => { applyHighlights(); rebuildEdges(); }, { deep: true });
watch(() => props.dimNonHighlighted, () => { applyHighlights(); rebuildEdges(); });
watch(() => props.colorRamp, () => applyHighlights());
</script>

<template>
  <div ref="container" class="graph3d"></div>
</template>

<style>
/* CSS2D labels — global scope so the renderer can find the class. */
.g3d-label {
  font-family: var(--font, system-ui);
  font-size: 11px;
  color: var(--text, #e6e8ee);
  background: rgba(14, 16, 20, 0.78);
  border: 1px solid rgba(38, 43, 54, 0.9);
  padding: 2px 7px;
  border-radius: 4px;
  white-space: nowrap;
  max-width: 320px;
  overflow: hidden;
  text-overflow: ellipsis;
  pointer-events: none;
  user-select: none;
  backdrop-filter: blur(4px);
  transform: translate(-50%, -100%);
}
</style>

<style scoped>
.graph3d { width: 100%; height: 100%; cursor: grab; position: relative; }
.graph3d:active { cursor: grabbing; }
</style>
