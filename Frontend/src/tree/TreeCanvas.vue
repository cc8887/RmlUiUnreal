<script setup lang="ts">
import { computed, ref, type RmlEvent } from '@rmlui/vue';
import { layoutTree, TREE_NODE_HEIGHT, TREE_NODE_WIDTH, type TreeCanvasNode } from './d3TreeLayout';

const props = defineProps<{ root: TreeCanvasNode }>();
const collapsed = ref<Set<string>>(new Set());
const selectedId = ref(props.root.id);
const zoom = ref(1);
const panX = ref(0);
const panY = ref(0);
const isPanning = ref(false);
let panStartX = 0;
let panStartY = 0;
let panOriginX = 0;
let panOriginY = 0;

const layout = computed(() => layoutTree(props.root, collapsed.value));
const selected = computed(() => layout.value.nodes.find(node => node.id === selectedId.value) ?? layout.value.nodes[0]);
const zoomLabel = computed(() => `${Math.round(zoom.value * 100)}%`);
const stageStyle = computed(() => ({
  width: `${layout.value.width}px`,
  height: `${layout.value.height}px`,
  transform: `translate(${panX.value}px, ${panY.value}px) scale(${zoom.value})`,
  'transform-origin': '0px 0px',
}));

function toggle(node: TreeCanvasNode): void {
  const next = new Set(collapsed.value);
  if (next.has(node.id)) next.delete(node.id); else next.add(node.id);
  collapsed.value = next;
}
function setZoom(next: number): void { zoom.value = Math.max(0.6, Math.min(1.2, next)); }
function resetView(): void { zoom.value = 1; panX.value = 0; panY.value = 0; }
function beginPan(event: RmlEvent): void {
  isPanning.value = true;
  panStartX = event.x; panStartY = event.y;
  panOriginX = panX.value; panOriginY = panY.value;
}
function movePan(event: RmlEvent): void {
  if (!isPanning.value) return;
  panX.value = panOriginX + event.x - panStartX;
  panY.value = panOriginY + event.y - panStartY;
}
function endPan(): void { isPanning.value = false; }
</script>

<template>
  <div id="tree-canvas" class="tree-canvas-page">
    <div class="tree-toolbar">
      <div><span class="tree-eyebrow">D3-HIERARCHY / NATIVE RMLUI</span><strong>Runtime architecture tree</strong></div>
      <div class="tree-actions">
        <button id="tree-zoom-out" class="tree-icon-button" title="Zoom out" @click="setZoom(zoom - 0.1)"><img src="icons/zoom-out.png" /></button>
        <span id="tree-zoom-value" class="tree-zoom-value">{{ zoomLabel }}</span>
        <button id="tree-zoom-in" class="tree-icon-button" title="Zoom in" @click="setZoom(zoom + 0.1)"><img src="icons/zoom-in.png" /></button>
        <button id="tree-reset-view" class="tree-reset-button" title="Reset view" @click="resetView"><img src="icons/maximize-2.png" /><span>Reset view</span></button>
      </div>
    </div>

    <div id="tree-viewport" class="tree-viewport" :class="isPanning ? 'panning' : ''">
      <div class="tree-pan-surface" @dragstart="beginPan" @drag="movePan" @dragend="endPan"></div>
      <div id="tree-stage" class="tree-stage" :data-pan-y="String(Math.round(panY))" :style="stageStyle" @dragstart.self="beginPan" @drag.self="movePan" @dragend.self="endPan">
        <div v-for="edge in layout.edges" :id="edge.id" :key="edge.id" class="tree-edge" :style="{ left: `${edge.left}px`, top: `${edge.top}px`, width: `${edge.width}px`, height: `${edge.height}px` }"></div>
        <div v-for="node in layout.nodes" :id="'tree-node-shell-' + node.id" :key="node.id" class="tree-node-shell" :style="{ left: `${node.left}px`, top: `${node.top}px`, width: `${TREE_NODE_WIDTH}px`, height: `${TREE_NODE_HEIGHT}px` }">
          <button :id="'tree-node-' + node.id" class="tree-node" :class="[node.kind, selectedId === node.id ? 'selected' : '']" @click.stop="selectedId = node.id">
            <strong>{{ node.label }}</strong><span>{{ node.detail }}</span>
          </button>
          <button v-if="node.hasChildren" :id="'tree-toggle-' + node.id" class="tree-toggle" :title="node.collapsed ? 'Expand branch' : 'Collapse branch'" @click.stop="toggle(node)">{{ node.collapsed ? '+' : '-' }}</button>
        </div>
      </div>
    </div>

    <div class="tree-status">
      <div><span>SELECTED</span><strong id="tree-selected-label">{{ selected.label }}</strong><small>{{ selected.detail }}</small></div>
      <div><span>VISIBLE NODES</span><strong id="tree-visible-count">{{ layout.nodes.length }}</strong><small>{{ collapsed.size }} collapsed branches</small></div>
      <div><span>LAYOUT ENGINE</span><strong id="tree-layout-engine">d3.tree()</strong><small>Tidy tree / native elements</small></div>
    </div>
  </div>
</template>

<style>
.tree-canvas-page { display:flex; flex-direction:column; min-height:0; flex:1; background-color:#e9eef0; }
.tree-toolbar { display:flex; align-items:center; justify-content:space-between; flex-shrink:0; min-height:58px; padding:10px 18px; border-bottom:1px #c6d0d5; background-color:#f9fafb; }
.tree-toolbar strong { margin-top:3px; color:#25343c; font-size:14px; }
.tree-eyebrow { color:#687985; font-family:"JetBrains Mono"; font-size:9px; }
.tree-actions { display:flex; align-items:center; gap:6px; }
.tree-icon-button { display:flex; align-items:center; justify-content:center; width:32px; height:32px; padding:0; border:1px #bcc8ce; border-radius:4px; background-color:#ffffff; }
.tree-icon-button:hover,.tree-reset-button:hover { border-color:#12846f; background-color:#e9f6f2; }
.tree-icon-button img,.tree-reset-button img { width:16px; height:16px; }
.tree-zoom-value { width:46px; color:#53636d; font-family:"JetBrains Mono"; font-size:10px; text-align:center; }
.tree-reset-button { display:flex; align-items:center; gap:7px; height:32px; padding:0 10px; border:1px #bcc8ce; border-radius:4px; background-color:#ffffff; color:#475760; font-size:10px; }
.tree-viewport { position:relative; min-height:0; flex:1; overflow:hidden; background-color:#e4eaed; }
.tree-pan-surface { position:absolute; top:0; right:0; bottom:0; left:0; background-color:#e4eaed; cursor:move; drag:drag; }
.tree-viewport.panning,.tree-viewport.panning .tree-pan-surface { cursor:move; }
.tree-stage { position:absolute; top:0; left:0; drag:drag; }
.tree-edge { position:absolute; background-color:#9aabb4; }
.tree-node-shell { position:absolute; }
.tree-node { display:flex; flex-direction:column; justify-content:center; box-sizing:border-box; width:176px; height:44px; padding:4px 30px 4px 12px; overflow:hidden; border:1px #b9c5cb; border-left:5px #55717e; border-radius:5px; background-color:#ffffff; color:#273740; text-align:left; box-shadow:0 3px 8px #26374122; }
.tree-node:hover { border-color:#168c78; background-color:#f6fbf9; }
.tree-node.selected { border-color:#087461; background-color:#e9f7f3; box-shadow:0 3px 10px #08746135; }
.tree-node.render { border-left-color:#3277a8; } .tree-node.bridge { border-left-color:#d69a2f; } .tree-node.tooling { border-left-color:#8b5e9d; }
.tree-node strong { overflow:hidden; font-size:12px; white-space:nowrap; }
.tree-node span { margin-top:4px; overflow:hidden; color:#71808a; font-family:"JetBrains Mono"; font-size:8px; white-space:nowrap; }
.tree-toggle { position:absolute; top:10px; right:8px; width:24px; height:24px; padding:0; border:1px #aab8bf; border-radius:3px; background-color:#edf2f3; color:#31505d; font-family:"JetBrains Mono"; font-size:14px; text-align:center; }
.tree-toggle:hover { border-color:#12846f; background-color:#dff2ec; color:#087461; }
.tree-status { display:grid; grid-template-columns:repeat(3,minmax(0px,1fr)); flex-shrink:0; gap:1px; min-height:68px; border-top:1px #c4cfd4; background-color:#c4cfd4; }
.tree-status div { min-width:0; padding:10px 16px; background-color:#f8fafb; }
.tree-status span { color:#71808a; font-family:"JetBrains Mono"; font-size:8px; }
.tree-status strong { margin-top:3px; overflow:hidden; color:#2c3d46; font-size:12px; white-space:nowrap; }
.tree-status small { display:block; margin-top:3px; overflow:hidden; color:#71808a; font-size:9px; white-space:nowrap; }
@media (max-width:760px) { .tree-toolbar { align-items:flex-start; flex-direction:column; gap:8px; } .tree-status { grid-template-columns:minmax(0px,1fr); } }
</style>
