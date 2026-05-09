<script setup>
import { ref, watch, onMounted, onBeforeUnmount, shallowRef } from 'vue';
import * as THREE from 'three';
import { OrbitControls } from 'three/addons/controls/OrbitControls.js';

const props = defineProps({
  nodes: { type: Array, default: () => [] },
  edges: { type: Array, default: () => [] },
  selectedId: { type: String, default: null },
  highlightedIds: { type: Array, default: () => [] },
  showSemantic: { type: Boolean, default: true },
  showKeyword:  { type: Boolean, default: true },
});
const emit = defineEmits(['select']);

const container = ref(null);

let renderer, scene, camera, controls, raycaster, mouse;
let nodeGroup, edgeGroup;
const nodeMeshes = shallowRef(new Map());   // id_hex -> Mesh
let animationFrame;

const COLOR = {
  bg:          0x0e1014,
  active:      0xc0caf5,
  superseded:  0x5b6478,
  selected:    0x7aa2f7,
  highlighted: 0xa6e3a1,
  edgeSemantic:0xbef264,
  edgeKeyword: 0x7dd3fc,
  edgeSupersedes: 0xf87171,
};

function disposeNodeMeshes() {
  if (!nodeGroup) return;
  while (nodeGroup.children.length) {
    const m = nodeGroup.children.pop();
    m.geometry?.dispose?.();
    m.material?.dispose?.();
  }
  nodeMeshes.value.clear();
}

function disposeEdgeLines() {
  if (!edgeGroup) return;
  while (edgeGroup.children.length) {
    const l = edgeGroup.children.pop();
    l.geometry?.dispose?.();
    l.material?.dispose?.();
  }
}

function rebuildNodes() {
  disposeNodeMeshes();
  const geom = new THREE.SphereGeometry(0.012, 16, 12);
  for (const n of props.nodes) {
    const mat = new THREE.MeshBasicMaterial({
      color: n.state === 'superseded' ? COLOR.superseded : COLOR.active,
    });
    const m = new THREE.Mesh(geom, mat);
    m.position.set(n.x, n.y, n.z);
    m.userData = { id: n.id_hex, title: n.title, state: n.state };
    nodeGroup.add(m);
    nodeMeshes.value.set(n.id_hex, m);
  }
}

function rebuildEdges() {
  disposeEdgeLines();
  const positions = { semantic: [], keyword: [], supersedes: [] };
  for (const e of props.edges) {
    const a = nodeMeshes.value.get(e.src);
    const b = nodeMeshes.value.get(e.dst);
    if (!a || !b) continue;
    if (e.kind === 'semantic' && !props.showSemantic) continue;
    if (e.kind === 'keyword' && !props.showKeyword) continue;
    const arr = positions[e.kind] || (positions[e.kind] = []);
    arr.push(a.position.x, a.position.y, a.position.z);
    arr.push(b.position.x, b.position.y, b.position.z);
  }
  const make = (arr, color, opacity) => {
    if (!arr.length) return;
    const g = new THREE.BufferGeometry();
    g.setAttribute('position', new THREE.Float32BufferAttribute(arr, 3));
    const m = new THREE.LineBasicMaterial({ color, transparent: true, opacity });
    const lines = new THREE.LineSegments(g, m);
    edgeGroup.add(lines);
  };
  make(positions.semantic,   COLOR.edgeSemantic,   0.55);
  make(positions.keyword,    COLOR.edgeKeyword,    0.45);
  make(positions.supersedes, COLOR.edgeSupersedes, 0.7);
}

function applyHighlights() {
  const sel = props.selectedId;
  const hi = new Set(props.highlightedIds || []);
  for (const [id, mesh] of nodeMeshes.value) {
    let color, scale = 1.0;
    if (id === sel) { color = COLOR.selected; scale = 1.6; }
    else if (hi.has(id)) { color = COLOR.highlighted; scale = 1.35; }
    else color = mesh.userData.state === 'superseded' ? COLOR.superseded : COLOR.active;
    mesh.material.color.setHex(color);
    mesh.scale.setScalar(scale);
  }
}

function frameToFit() {
  if (!props.nodes.length) return;
  const box = new THREE.Box3();
  for (const n of props.nodes) {
    box.expandByPoint(new THREE.Vector3(n.x, n.y, n.z));
  }
  const size = new THREE.Vector3();
  box.getSize(size);
  const center = new THREE.Vector3();
  box.getCenter(center);
  const radius = Math.max(size.x, size.y, size.z, 0.5);
  camera.position.copy(center).add(new THREE.Vector3(radius * 1.6, radius * 1.2, radius * 2.0));
  controls.target.copy(center);
  controls.update();
}

function onClick(ev) {
  const rect = renderer.domElement.getBoundingClientRect();
  mouse.x = ((ev.clientX - rect.left) / rect.width) * 2 - 1;
  mouse.y = -((ev.clientY - rect.top) / rect.height) * 2 + 1;
  raycaster.setFromCamera(mouse, camera);
  const hits = raycaster.intersectObjects(nodeGroup.children, false);
  if (hits.length) emit('select', hits[0].object.userData.id);
}

function onResize() {
  const w = container.value.clientWidth;
  const h = container.value.clientHeight;
  renderer.setSize(w, h, false);
  camera.aspect = w / h;
  camera.updateProjectionMatrix();
}

function loop() {
  controls.update();
  renderer.render(scene, camera);
  animationFrame = requestAnimationFrame(loop);
}

onMounted(() => {
  scene = new THREE.Scene();
  scene.background = new THREE.Color(COLOR.bg);

  const w = container.value.clientWidth;
  const h = container.value.clientHeight;
  camera = new THREE.PerspectiveCamera(55, w / h, 0.001, 100);
  camera.position.set(1.2, 0.9, 1.6);

  renderer = new THREE.WebGLRenderer({ antialias: true });
  renderer.setPixelRatio(window.devicePixelRatio);
  renderer.setSize(w, h);
  container.value.appendChild(renderer.domElement);

  controls = new OrbitControls(camera, renderer.domElement);
  controls.enableDamping = true;
  controls.dampingFactor = 0.12;

  nodeGroup = new THREE.Group();
  edgeGroup = new THREE.Group();
  scene.add(edgeGroup);
  scene.add(nodeGroup);

  raycaster = new THREE.Raycaster();
  raycaster.params.Points = { threshold: 0.02 };
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
  disposeNodeMeshes();
  disposeEdgeLines();
  controls?.dispose();
  renderer?.dispose();
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

<style scoped>
.graph3d { width: 100%; height: 100%; cursor: grab; }
.graph3d:active { cursor: grabbing; }
</style>
