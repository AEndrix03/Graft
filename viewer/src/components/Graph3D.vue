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
});
const emit = defineEmits(['select', 'select-edge']);

const container = ref(null);

let renderer, labelRenderer, scene, camera, controls, raycaster, mouse;
let nodeGroup, edgeGroup;
const nodeMeshes = shallowRef(new Map());     // id_hex -> Mesh
const nodeLabels = shallowRef(new Map());     // id_hex -> CSS2DObject
let edgeLineObjects = [];                     // { obj, perSegment: edge[] }
let animationFrame;

/* Tunables */
const SPATIAL_SCALE     = 6.0;   // multiplier on the projected coords (more breathing room)
const NODE_R_BASE       = 0.018; // sphere radius for an "average" body
const NODE_R_MIN        = 0.012;
const NODE_R_MAX        = 0.06;
const NODE_R_LOG_FACTOR = 0.014; // delta per ln(body_len)
const LABEL_NEAR_DIST   = 2.5;   // labels visible if camera within this distance of node (after scale)
const EDGE_LINEWIDTH    = 2.5;   // pixel-thick edges via Line2

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
    const geom = new THREE.SphereGeometry(r, 18, 14);
    const mat = new THREE.MeshBasicMaterial({ color: nodeColor(n) });
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

function rebuildEdges() {
  // Dispose previous line objects
  for (const e of edgeLineObjects) {
    edgeGroup.remove(e.obj);
    e.obj.geometry?.dispose?.();
    e.obj.material?.dispose?.();
  }
  edgeLineObjects = [];

  const buckets = {
    semantic:   { positions: [], edges: [], color: COLOR.edgeSemantic,   opacity: 0.7 },
    keyword:    { positions: [], edges: [], color: COLOR.edgeKeyword,    opacity: 0.55 },
    supersedes: { positions: [], edges: [], color: COLOR.edgeSupersedes, opacity: 0.85 },
  };

  for (const e of props.edges) {
    const a = nodeMeshes.value.get(e.src);
    const b = nodeMeshes.value.get(e.dst);
    if (!a || !b) continue;
    if (e.kind === 'semantic' && !props.showSemantic) continue;
    if (e.kind === 'keyword' && !props.showKeyword) continue;
    const bucket = buckets[e.kind];
    if (!bucket) continue;
    bucket.positions.push(
      a.position.x, a.position.y, a.position.z,
      b.position.x, b.position.y, b.position.z,
    );
    bucket.edges.push(e);
  }

  const w = renderer.domElement.clientWidth;
  const h = renderer.domElement.clientHeight;
  for (const k of Object.keys(buckets)) {
    const b = buckets[k];
    if (!b.positions.length) continue;
    const geom = new LineSegmentsGeometry();
    geom.setPositions(b.positions);
    const mat = new LineMaterial({
      color: b.color,
      linewidth: EDGE_LINEWIDTH,
      transparent: true,
      opacity: b.opacity,
      worldUnits: false,
      dashed: false,
    });
    mat.resolution.set(w, h);
    const lines = new LineSegments2(geom, mat);
    lines.computeLineDistances();
    lines.userData = { kind: k, segments: b.edges };
    edgeGroup.add(lines);
    edgeLineObjects.push({ obj: lines, perSegment: b.edges });
  }
}

function applyHighlights() {
  const sel = props.selectedId;
  const hi = new Set(props.highlightedIds || []);
  for (const [id, mesh] of nodeMeshes.value) {
    let color, scale = 1.0;
    if (id === sel)        { color = new THREE.Color(COLOR.selected);    scale = 1.55; }
    else if (hi.has(id))   { color = new THREE.Color(COLOR.highlighted); scale = 1.30; }
    else                   { color = mesh.userData.baseColor; }
    mesh.material.color.copy(color);
    mesh.scale.setScalar(scale);
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

  nodeGroup = new THREE.Group();
  edgeGroup = new THREE.Group();
  scene.add(edgeGroup);
  scene.add(nodeGroup);

  raycaster = new THREE.Raycaster();
  raycaster.params.Line2 = { threshold: 6 };
  mouse = new THREE.Vector2();

  renderer.domElement.addEventListener('click', onClick);
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
watch(() => props.selectedId,    applyHighlights);
watch(() => props.highlightedIds, applyHighlights, { deep: true });
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
