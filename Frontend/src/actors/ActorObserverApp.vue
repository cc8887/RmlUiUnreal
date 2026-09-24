<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref, type RmlEvent } from '@rmlui/vue';
import { getService } from '../bridge';
import RmlEChart from '../charts/RmlEChart.vue';
import type { EditableChartDatum } from '../charts/echartsSvg';
import TreeCanvas from '../tree/TreeCanvas.vue';
import type { TreeCanvasNode } from '../tree/d3TreeLayout';
import DialogShowcase from '../dialogs/DialogShowcase.vue';
import GoldEdgeMask from '../mask/GoldEdgeMask.vue';
import HostShowcase from '../dialogs/HostShowcase.vue';
import HeadlessDataShowcase from '../data/HeadlessDataShowcase.vue';
import SceneOverlayShowcase from '../scene/SceneOverlayShowcase.vue';
import TalentTreeView from './TalentTreeView.vue';
import MotionDropdownShowcase from './MotionDropdownShowcase.vue';
import AnimationSpringShowcase from './AnimationSpringShowcase.vue';
import AnimationOfficialExamples from './AnimationOfficialExamples.vue';
import NativeCssAnimationShowcase from './NativeCssAnimationShowcase.vue';
import CssProbeView from './CssProbeView.vue';

interface ActorSummary { path: string; name: string; type: string; level: string; hidden: boolean; ticking: boolean }
interface ActorProperty { category: string; name: string; type: string; value: string }
interface ActorDetails { found: boolean; path?: string; name?: string; type?: string; level?: string; transform?: { location: string; rotation: string; scale: string }; properties?: ActorProperty[] }
interface ActorSnapshot { sequence: number; level: string; actors: ActorSummary[] }
interface ActorObserverService { GetActorSnapshot(): string; GetActorDetails(actorPath: string): string; SetUiMaterialIntensity(intensity: number): string }

const observer = getService<ActorObserverService>('actorObserver');
const snapshot = ref<ActorSnapshot>({ sequence: 0, level: '', actors: [] });
const selectedPath = ref('');
const showDetails = ref(false);
const details = ref<ActorDetails | null>(null);
const query = ref('');
const activeView = ref<'actors' | 'lab' | 'tree' | 'dialogs' | 'mask' | 'host' | 'data' | 'scene' | 'talent' | 'motion' | 'animation' | 'animationofficial' | 'cssmotion' | 'cssprobe'>('cssmotion');
const isLive = ref(true);
const error = ref('');
const materialIntensity = ref(72);
const materialStatus = ref('UE MID / 72%');
const maskProbeCount = ref(0);
const chartData = ref<EditableChartDatum[]>([
  { label: 'Render', value: 68, target: 82 },
  { label: 'Layout', value: 54, target: 72 },
  { label: 'Script', value: 76, target: 88 },
  { label: 'Input', value: 43, target: 66 },
  { label: 'Memory', value: 61, target: 75 },
]);
const architectureTree: TreeCanvasNode = {
  id: 'rmlui-unreal', label: 'RmlUiUnreal', detail: 'UE 5.8 plugin', kind: 'runtime', children: [
    { id: 'runtime', label: 'Runtime UI', detail: 'layout and rendering', kind: 'runtime', children: [
      { id: 'rmlui-core', label: 'RmlUi Core', detail: 'RML / RCSS / Grid', kind: 'runtime' },
      { id: 'slate-rhi', label: 'Slate / RHI', detail: 'native draw commands', kind: 'render' },
      { id: 'svg-plugin', label: 'SVG Plugin', detail: 'LunaSVG textures', kind: 'render' },
    ] },
    { id: 'application', label: 'Application Layer', detail: 'typed reactive UI', kind: 'bridge', children: [
      { id: 'vue-renderer', label: 'Vue Renderer', detail: 'runtime-core nodes', kind: 'bridge' },
      { id: 'puerts-services', label: 'Puerts Services', detail: 'typed UObject calls', kind: 'bridge', children: [
        { id: 'actor-observer', label: 'Actor Observer', detail: 'Editor World service', kind: 'tooling' },
      ] },
      { id: 'echarts-ssr', label: 'ECharts SSR', detail: 'DOM-free SVG output', kind: 'render' },
    ] },
    { id: 'tooling', label: 'Delivery Tooling', detail: 'build and validation', kind: 'tooling', children: [
      { id: 'bundles', label: 'Content Bundles', detail: 'manifest and hashes', kind: 'tooling' },
      { id: 'automation', label: 'Automation', detail: 'DX11 / DX12 smoke', kind: 'tooling' },
      { id: 'hot-reload', label: 'Hot Reload', detail: 'atomic activation', kind: 'tooling' },
    ] },
  ],
};
const textPaneHeight = ref(190);
const isResizingText = ref(false);
let resizeStartY = 0;
let resizeStartHeight = 190;
let timer = 0;

const typeCount = computed(() => new Set(snapshot.value.actors.map(actor => actor.type)).size);
const visibleActors = computed(() => {
  const needle = query.value.trim().toLowerCase();
  return snapshot.value.actors.filter(actor => !needle || actor.name.toLowerCase().includes(needle) || actor.type.toLowerCase().includes(needle));
});
const selected = computed(() => snapshot.value.actors.find(actor => actor.path === selectedPath.value));
const tickingCount = computed(() => snapshot.value.actors.filter(actor => actor.ticking).length);
const liveLabel = computed(() => isLive.value ? 'Live / 500 ms' : 'Paused');

function parse<T>(text: string): T { return JSON.parse(text) as T; }
function refresh(): void {
  try {
    const next = parse<ActorSnapshot>(observer.GetActorSnapshot());
    snapshot.value = next;
    if (selectedPath.value && !next.actors.some(actor => actor.path === selectedPath.value)) {
      selectedPath.value = '';
      showDetails.value = false;
      details.value = null;
    }
    if (showDetails.value && selectedPath.value) details.value = parse<ActorDetails>(observer.GetActorDetails(selectedPath.value));
    error.value = '';
  } catch (caught) { error.value = caught instanceof Error ? caught.message : String(caught); }
}
function select(actor: ActorSummary): void {
  selectedPath.value = actor.path;
  if (showDetails.value) details.value = parse<ActorDetails>(observer.GetActorDetails(actor.path));
}
function toggleDetails(): void {
  showDetails.value = !showDetails.value;
  details.value = showDetails.value && selectedPath.value ? parse<ActorDetails>(observer.GetActorDetails(selectedPath.value)) : null;
}
function toggleLive(): void {
  isLive.value = !isLive.value;
  if (isLive.value) refresh();
}
function beginTextResize(event: RmlEvent): void {
  isResizingText.value = true;
  resizeStartY = event.y;
  resizeStartHeight = textPaneHeight.value;
}
function resizeText(event: RmlEvent): void {
  if (!isResizingText.value) return;
  textPaneHeight.value = Math.max(120, Math.min(270, resizeStartHeight + event.y - resizeStartY));
}
function endTextResize(): void { isResizingText.value = false; }
function updateMaterial(): void { materialStatus.value = observer.SetUiMaterialIntensity(Math.round(materialIntensity.value)); }
onMounted(() => { refresh(); updateMaterial(); timer = setInterval(() => { if (isLive.value) refresh(); }, 500) as unknown as number; });
onUnmounted(() => clearInterval(timer));
</script>

<template>
  <div class="observer-shell flex h-full w-full flex-col text-ink" :class="activeView === 'scene' ? 'scene-mode' : ''">
    <div class="topbar flex items-center justify-between border-b px-6 py-4">
      <div class="flex flex-col gap-1">
        <h1 class="m-0 text-xl font-bold">Level Actor Observer</h1>
        <p class="m-0 text-xs text-slate-500">{{ snapshot.level || 'World unavailable' }}</p>
      </div>
      <div class="flex items-center gap-3">
        <div class="live-state flex items-center gap-2 text-xs" :class="isLive ? 'running' : 'paused'"><span class="live-dot"></span><span id="actor-live-status">{{ liveLabel }}</span></div>
        <button id="actor-refresh" class="icon-button" :class="isLive ? '' : 'paused'" :title="isLive ? 'Pause live refresh' : 'Resume live refresh'" @click="toggleLive"><img :src="isLive ? 'icons/pause.png' : 'icons/play.png'" /></button>
      </div>
    </div>

    <div class="view-tabs flex items-center border-b px-6">
      <button id="actor-view-tab" class="view-tab" :class="activeView === 'actors' ? 'active' : ''" @click="activeView = 'actors'"><img src="icons/list-tree.png" /><span>Actors</span></button>
      <button id="ui-lab-tab" class="view-tab" :class="activeView === 'lab' ? 'active' : ''" @click="activeView = 'lab'"><img src="icons/panels-top-left.png" /><span>UI Lab</span></button>
      <button id="motion-view-tab" class="view-tab" :class="activeView === 'motion' ? 'active' : ''" @click="activeView = 'motion'"><img src="icons/chevron-down.png" /><span>Motion Menu</span></button>
      <button id="tree-view-tab" class="view-tab" :class="activeView === 'tree' ? 'active' : ''" @click="activeView = 'tree'"><img src="icons/git-branch.png" /><span>Tree Canvas</span></button>
      <button id="dialogs-view-tab" class="view-tab" :class="activeView === 'dialogs' ? 'active' : ''" @click="activeView = 'dialogs'"><img src="icons/app-window.png" /><span>Dialogs</span></button>
      <button id="mask-view-tab" class="view-tab" :class="activeView === 'mask' ? 'active' : ''" @click="activeView = 'mask'"><img src="icons/maximize-2.png" /><span>Mask Frame</span></button>
      <button id="host-view-tab" class="view-tab" :class="activeView === 'host' ? 'active' : ''" @click="activeView = 'host'"><img src="icons/settings-2.png" /><span>Interaction Lab</span></button>
      <button id="data-view-tab" class="view-tab" :class="activeView === 'data' ? 'active' : ''" @click="activeView = 'data'"><img src="icons/list-tree.png" /><span>Headless Data</span></button>
      <button id="scene-view-tab" class="view-tab" :class="activeView === 'scene' ? 'active' : ''" @click="activeView = 'scene'"><img src="icons/eye.png" /><span>Scene Overlay</span></button>
      <button id="talent-view-tab" class="view-tab" :class="activeView === 'talent' ? 'active' : ''" @click="activeView = 'talent'"><img src="icons/sparkles.png" /><span>Talent Tree</span></button>
      <button id="animation-view-tab" class="view-tab" :class="activeView === 'animation' ? 'active' : ''" @click="activeView = 'animation'"><img src="icons/rotate-ccw.png" /><span>Spring Reveal</span></button>
      <button id="animation-official-view-tab" class="view-tab" :class="activeView === 'animationofficial' ? 'active' : ''" @click="activeView = 'animationofficial'"><img src="icons/play.png" /><span>Animation.js</span></button>
      <button id="css-motion-view-tab" class="view-tab" :class="activeView === 'cssmotion' ? 'active' : ''" @click="activeView = 'cssmotion'"><img src="icons/sparkles.png" /><span>CSS Motion</span></button>
<button id="cssprobe-view-tab" class="view-tab" :class="activeView === 'cssprobe' ? 'active' : ''" @click="activeView = 'cssprobe'"><img src="icons/list-tree.png" /><span>CSS Probe</span></button>
    </div>

    <template v-if="activeView === 'actors'">
    <div class="metrics grid grid-cols-3 gap-4 border-b px-6 py-4">
      <div><p class="metric-label">Actors</p><p id="actor-count" class="metric-value">{{ snapshot.actors.length }}</p></div>
      <div><p class="metric-label">Types</p><p class="metric-value">{{ typeCount }}</p></div>
      <div><p class="metric-label">Tick enabled</p><p class="metric-value">{{ tickingCount }}</p></div>
    </div>

    <div class="grid grid-cols-2 gap-3 border-b border-slate-300 bg-white px-6 py-3">
      <input id="actor-search" v-model="query" class="control" type="text" placeholder="Filter by name or type" />
      <div class="control flex items-center text-xs text-slate-500">Showing {{ visibleActors.length }} / {{ snapshot.actors.length }}</div>
    </div>

    <div class="flex min-h-0 flex-1 flex-col bg-white">
      <div class="actor-heading grid grid-cols-12 gap-3 border-b border-slate-300 bg-slate-100 px-6 py-2 text-xs font-bold text-slate-500">
        <span class="col-span-5">Name</span><span class="col-span-4">Type</span><span class="col-span-2">Level</span><span class="col-span-1">Tick</span>
      </div>
      <div id="actor-list" class="min-h-0 flex-1 overflow-auto">
        <button v-for="actor in visibleActors" :id="'actor-row-' + actor.path" :key="actor.path" class="actor-row grid w-full grid-cols-12 gap-3 border-b border-slate-200 px-6 py-3 text-left" :class="selectedPath === actor.path ? 'selected' : ''" @click="select(actor)">
          <span class="col-span-5 font-bold">{{ actor.name }}</span>
          <span class="col-span-4 text-slate-600">{{ actor.type }}</span>
          <span class="col-span-2 text-slate-500">{{ actor.level }}</span>
          <span class="col-span-1" :class="actor.ticking ? 'text-signal' : 'text-slate-400'">{{ actor.ticking ? 'On' : '-' }}</span>
        </button>
        <p v-if="visibleActors.length === 0" class="m-6 text-sm text-slate-500">No matching actors</p>
      </div>
    </div>

    <div class="border-t border-slate-300 bg-slate-50">
      <div class="flex items-center justify-between px-6 py-3">
        <div><p class="m-0 text-sm font-bold">{{ selected ? selected.name : 'No actor selected' }}</p><p class="m-0 text-xs text-slate-500">{{ selected ? selected.type : 'Select a row to inspect it' }}</p></div>
        <button id="actor-details-toggle" class="detail-toggle flex items-center gap-2" :class="showDetails ? 'active' : ''" :disabled="!selected" @click="toggleDetails">
          <img src="icons/panel-bottom.png" /><span>{{ showDetails ? 'Hide details' : 'Show details' }}</span>
        </button>
      </div>
      <div v-if="showDetails && details && details.found" id="actor-details" class="details-pane border-t border-slate-300 bg-white px-6 py-4">
        <div class="grid grid-cols-3 gap-4 border-b border-slate-200 pb-3">
          <div><p class="detail-label">Location</p><p id="actor-detail-location" class="mono">{{ details.transform?.location }}</p></div>
          <div><p class="detail-label">Rotation</p><p class="mono">{{ details.transform?.rotation }}</p></div>
          <div><p class="detail-label">Scale</p><p class="mono">{{ details.transform?.scale }}</p></div>
        </div>
        <div class="property-list pt-2">
          <div v-for="property in details.properties" :key="property.category + property.name" class="property-row grid grid-cols-12 gap-3 border-b border-slate-100 py-2 text-xs">
            <span class="col-span-3 text-slate-500">{{ property.category }}</span><span class="col-span-3 font-bold">{{ property.name }}</span><span class="col-span-2 text-warning">{{ property.type }}</span><span class="mono col-span-4">{{ property.value }}</span>
          </div>
        </div>
      </div>
      <p v-if="error" id="actor-error" class="m-0 border-t border-red-300 bg-red-50 px-6 py-2 text-xs text-red-700">{{ error }}</p>
    </div>
    </template>

    <div v-else-if="activeView === 'lab'" id="ui-lab" class="lab-scroll min-h-0 flex-1 overflow-auto">
      <div class="lab-grid">
        <section class="lab-panel icon-panel">
          <div class="panel-heading"><span class="eyebrow">ICON SYSTEM</span><strong>Editor actions</strong></div>
          <div class="icon-grid">
            <div class="icon-sample"><img src="icons/search.png" /><span>Search</span></div>
            <div class="icon-sample"><img src="icons/circle-plus.png" /><span>Create</span></div>
            <div class="icon-sample"><img src="icons/eye.png" /><span>Inspect</span></div>
            <div class="icon-sample"><img src="icons/settings-2.png" /><span>Settings</span></div>
            <div class="icon-sample"><img src="icons/save.png" /><span>Save</span></div>
            <div class="icon-sample"><img src="icons/trash-2.png" /><span>Delete</span></div>
          </div>
        </section>

        <section class="lab-panel image-panel">
          <div class="panel-heading"><span class="eyebrow">IMAGE RESOURCE</span><strong>RmlUi sample texture</strong></div>
          <div class="image-stage">
            <img id="ui-lab-image" class="sample-image" src="hello_world.png" />
            <span class="image-anchor top-right">TOP / RIGHT</span>
            <span class="image-anchor bottom-left">BOTTOM / LEFT</span>
          </div>
        </section>

        <section class="lab-panel effects-panel">
          <div class="panel-heading"><span class="eyebrow">SURFACE & MOTION</span><strong>Shape, depth and animation</strong></div>
          <div class="effect-grid">
            <div class="rounding-demo"><span class="radius-small">2</span><span class="radius-medium">6</span><span class="radius-pill">Pill</span></div>
            <div id="ui-lab-shadow" class="shadow-demo"><div class="shadow-sheet">Soft shadow</div></div>
            <div id="ui-lab-motion" class="motion-demo"><div class="motion-track"><span class="motion-scan"></span></div><div class="motion-square"></div></div>
            <div class="swatches"><span class="swatch teal"></span><span class="swatch blue"></span><span class="swatch amber"></span><span class="swatch coral"></span><span class="swatch violet"></span></div>
          </div>
        </section>

        <section class="lab-panel layout-panel">
          <div class="panel-heading"><span class="eyebrow">GRID & ANCHORS</span><strong>Complex responsive layout</strong></div>
          <div id="ui-lab-layout" class="layout-stage">
            <div class="layout-header">HEADER</div>
            <div class="layout-side">SIDE</div>
            <div class="layout-main">MAIN</div>
            <div class="layout-foot">FOOTER</div>
            <span class="anchor-badge anchor-ne">NE</span>
            <span class="anchor-badge anchor-sw">SW</span>
          </div>
        </section>

        <section class="lab-panel material-panel">
          <div class="panel-heading"><span class="eyebrow">UNREAL UI MATERIAL / SLATE</span><strong id="ui-material-status">{{ materialStatus }}</strong></div>
          <div class="material-workspace">
            <div id="ui-material-preview" class="material-preview">
              <div class="material-preview-copy"><span>HOST-REGISTERED ALIAS</span><strong>showcase.energy</strong><p>UMaterial → MID → FSlateMaterialBrush</p></div>
              <div class="material-meter"><span :style="{ width: `${materialIntensity}%` }"></span></div>
            </div>
            <div class="material-controls">
              <span class="material-label">TINT INTENSITY</span>
              <strong id="ui-material-value">{{ materialIntensity }}%</strong>
              <input id="ui-material-slider" v-model.number="materialIntensity" class="chart-slider" type="range" min="0" max="100" step="1" @input="updateMaterial" />
              <p>The native range input updates the Unreal MID vector parameter through the typed Puerts service.</p>
            </div>
          </div>
        </section>

        <section class="lab-panel chart-panel">
          <div class="panel-heading"><span class="eyebrow">APACHE ECHARTS / SVG SSR</span><strong>Live performance model</strong></div>
          <div class="chart-workspace">
            <div id="ui-lab-echarts" class="chart-surface"><RmlEChart :data="chartData" /></div>
            <div id="ui-lab-chart-table" class="chart-table">
              <div class="chart-row chart-header"><span>Metric</span><span>Current</span><span>Target</span><span>Adjust</span></div>
              <div v-for="(item, index) in chartData" :id="'chart-row-' + index" :key="item.label" class="chart-row">
                <strong>{{ item.label }}</strong>
                <span :id="'chart-value-' + index" class="chart-value">{{ item.value }}</span>
                <span class="chart-target">{{ item.target }}</span>
                <input :id="'chart-slider-' + index" v-model.number="item.value" class="chart-slider" type="range" min="0" max="100" step="1" />
              </div>
            </div>
          </div>
        </section>

        <section class="lab-panel text-panel">
          <div class="panel-heading"><span class="eyebrow">RESIZABLE SPLIT VIEW</span><strong id="text-pane-size">Text pane / {{ textPaneHeight }} px</strong></div>
          <div id="ui-lab-split" class="split-shell">
            <div id="ui-lab-scroll" class="text-scroll" :style="{ height: `${textPaneHeight}px` }">
              <h2>World observation report</h2>
              <p class="text-lead">A long-form panel can keep dense reference material inside the editor without expanding the surrounding tool. This specimen uses the native RmlUi scroll container and styled vertical scrollbar.</p>
              <p><strong>Snapshot lifecycle.</strong> The observer requests a compact representation of the current Editor World on a fixed interval. Each snapshot contains stable object paths so a selected Actor remains selected while rows are reordered or other Actors are added.</p>
              <p><strong>Selection behavior.</strong> The list and detail surface have separate responsibilities. Row updates stay lightweight, while reflected properties are requested only when the details area is visible and an Actor is selected.</p>
              <p><strong>Editor world changes.</strong> Opening another level replaces the active Editor World. The editor panel detects that change and reinitializes its service before the next frontend snapshot is requested.</p>
              <p><strong>Presentation.</strong> Headings, emphasized runs, paragraphs and monospace metadata can coexist in one constrained reading surface. Padding keeps glyphs clear of the scrollbar and rounded clipping contains the scrolled content.</p>
              <p><strong>Operational boundary.</strong> Live polling is appropriate for this compact demonstration, but large partitioned worlds should move toward event-driven indexing, virtualized rows and measured Game Thread budgets.</p>
              <p><strong>End of sample.</strong> Reaching this paragraph confirms that the container can expose content beyond its initial viewport while the rest of the UI Lab remains fixed and independently scrollable.</p>
            </div>
            <div id="ui-lab-resizer" class="split-handle" :class="isResizingText ? 'active' : ''" @dragstart="beginTextResize" @drag="resizeText" @dragend="endTextResize"><span class="split-grip"></span></div>
            <div id="ui-lab-lower" class="split-lower">
              <div class="split-lower-heading"><span>REMAINING LAYOUT</span><span>Editor viewport</span></div>
              <div class="split-summary">
                <div><span class="summary-label">WORLD</span><strong>{{ snapshot.level || 'Unavailable' }}</strong></div>
                <div><span class="summary-label">SELECTION</span><strong>{{ selected ? selected.name : 'None' }}</strong></div>
                <div><span class="summary-label">RENDERER</span><strong>RmlUi / Slate</strong></div>
              </div>
            </div>
          </div>
        </section>
      </div>
    </div>
    <TreeCanvas v-else-if="activeView === 'tree'" :root="architectureTree" />
    <DialogShowcase v-else-if="activeView === 'dialogs'" />
    <HostShowcase v-else-if="activeView === 'host'" />
    <HeadlessDataShowcase v-else-if="activeView === 'data'" />
    <SceneOverlayShowcase v-else-if="activeView === 'scene'" />
    <TalentTreeView v-else-if="activeView === 'talent'" />
<MotionDropdownShowcase v-else-if="activeView === 'motion'" />
<AnimationSpringShowcase v-else-if="activeView === 'animation'" />
<AnimationOfficialExamples v-else-if="activeView === 'animationofficial'" />
<NativeCssAnimationShowcase v-else-if="activeView === 'cssmotion'" />
<CssProbeView v-else-if="activeView === 'cssprobe'" />
    <div v-else id="mask-showcase" class="mask-showcase">
      <div class="mask-kicker">LUNASVG / SVG MASK</div>
      <h2>Screen Edge Frame</h2>
      <p>RmlUi glow layers, moving energy and a transparent center.</p>
      <div class="mask-contract"><span>MASK</span><strong id="mask-engine">SVG &lt;mask&gt;</strong><span>MOTION</span><strong id="mask-motion">Native animation</strong><span>INPUT</span><strong>Pass-through</strong></div>
      <button id="mask-probe" class="mask-probe" @click="maskProbeCount++">Probe input <strong id="mask-probe-count">{{ maskProbeCount }}</strong></button>
    </div>
    <GoldEdgeMask v-if="activeView === 'mask'" />
  </div>
</template>

<style>
@tailwind utilities;
body { margin:0; width:100%; height:100%; overflow:hidden; background-color:transparent; font-family:"Noto Sans CJK SC"; font-size:14px; }
div,h1,h2,p,span,section,strong { display:block; } button,img,input { display:block; }
button { cursor:pointer; font-family:"Noto Sans CJK SC"; } button:disabled { cursor:default; opacity:0.45; }
.observer-shell { background-color:#eef1f3; }
.observer-shell.scene-mode { background-color:transparent; }
.scene-mode .topbar { background-color:#eef5f2dd; }
.scene-mode .view-tabs { background-color:#edf3f0dd; }
.scene-mode .view-tab.active { background-color:#ffffffbb; }
.topbar { border-color:#cbd2d8; background-color:#f9fafb; box-shadow:0 2px 8px #18242e18; }
.live-state { color:#677580; } .live-state.running { color:#087f69; } .live-state.paused { color:#a76314; }
.live-dot { width:7px; height:7px; border-radius:7px; background-color:#9aa6af; } .running .live-dot { background-color:#0d9b7d; animation:1.2s cubic-in-out live-pulse infinite alternate; } .paused .live-dot { background-color:#d18b2c; }
@keyframes live-pulse { from { opacity:0.35; transform:scale(0.8); } to { opacity:1; transform:scale(1.15); } }
.view-tabs { height:42px; overflow-x:auto; overflow-y:hidden; background-color:#f4f6f7; border-color:#cbd2d8; }
.view-tab { display:flex; flex-shrink:0; align-items:center; gap:7px; height:42px; padding:0 16px; border:0; border-bottom:3px transparent; border-radius:0; background-color:transparent; color:#64717c; white-space:nowrap; }
.view-tab.active { border-bottom:3px #0d8a73; color:#17252d; background-color:#ffffff; } .view-tab img { width:16px; height:16px; }
.metrics { border-color:#d2d8dd; background-color:#e9eef0; }
.metric-label,.detail-label { margin:0; color:#64748b; font-size:11px; } .metric-value { margin:4px 0 0; font-family:"JetBrains Mono"; font-size:24px; }
.control { height:36px; box-sizing:border-box; padding:7px 10px; border:1px #cbd5e1; border-radius:4px; background-color:#ffffff; color:#202a2e; }
.icon-button { display:flex; align-items:center; justify-content:center; box-sizing:border-box; width:34px; height:34px; padding:0; border:1px #bdc7ce; border-radius:4px; background-color:#ffffff; }
.icon-button:hover { border-color:#0d8a73; background-color:#e8f5f1; } .icon-button.paused { border-color:#d6a457; background-color:#fff7e8; }
.icon-button img { width:18px; height:18px; }
.actor-row { min-height:43px; box-sizing:border-box; border-top-width:0; border-left-width:0; border-right-width:0; border-radius:0; background-color:#ffffff; color:#202a2e; font-size:12px; }
.actor-row:hover { background-color:#f8fafc; } .actor-row.selected { border-left:4px #14866d; background-color:#ecfdf5; padding-left:20px; }
.detail-toggle { min-width:132px; height:34px; padding:7px 11px; border:1px #cbd5e1; border-radius:4px; background-color:#ffffff; color:#334155; }
.detail-toggle.active { border-color:#14866d; background-color:#ecfdf5; color:#116b58; } .detail-toggle img { width:17px; height:17px; }
.details-pane { max-height:270px; overflow:auto; } .property-list { max-height:190px; overflow:auto; } .property-row { min-height:30px; }
.mono { margin:3px 0 0; font-family:"JetBrains Mono"; font-size:11px; word-break:break-all; }
.lab-scroll { background-color:#e7ebee; }
.lab-grid { display:grid; grid-template-columns:minmax(0px,7fr) minmax(280px,5fr); grid-template-areas:"icons image" "effects layout" "material material" "chart chart" "text text"; gap:14px; padding:16px; }
.lab-panel { min-width:0; padding:16px; border:1px #c8d0d6; border-radius:6px; background-color:#f9fafb; box-shadow:0 5px 14px #26354120; }
.icon-panel { grid-area:icons; } .image-panel { grid-area:image; } .effects-panel { grid-area:effects; } .layout-panel { grid-area:layout; } .material-panel { grid-area:material; } .chart-panel { grid-area:chart; } .text-panel { grid-area:text; }
.panel-heading { display:flex; justify-content:space-between; align-items:center; margin-bottom:14px; } .panel-heading strong { font-size:14px; color:#26343c; } .eyebrow { font-family:"JetBrains Mono"; font-size:10px; color:#74828c; }
.icon-grid { display:grid; grid-template-columns:repeat(3,minmax(0px,1fr)); gap:8px; }
.icon-sample { display:flex; align-items:center; gap:9px; min-height:46px; padding:8px 10px; border:1px #d4dade; border-radius:4px; background-color:#ffffff; color:#4d5b65; font-size:11px; }
.icon-sample img { width:20px; height:20px; }
.image-stage { position:relative; height:156px; overflow:hidden; border-radius:6px 18px 6px 18px; background-color:#dfe5e8; }
.sample-image { width:100%; height:auto; }
.image-anchor { position:absolute; padding:5px 7px; border-radius:3px; background-color:#172a34dd; color:#ffffff; font-family:"JetBrains Mono"; font-size:9px; }
.top-right { top:8px; right:8px; } .bottom-left { bottom:8px; left:8px; background-color:#b9523ddd; }
.effect-grid { display:grid; grid-template-columns:repeat(2,minmax(0px,1fr)); gap:10px; }
.rounding-demo,.shadow-demo,.motion-demo,.swatches { min-height:76px; box-sizing:border-box; padding:12px; border:1px #d6dde1; background-color:#eef2f4; }
.rounding-demo { display:flex; align-items:center; justify-content:space-between; } .rounding-demo span { padding:8px 10px; background-color:#256d85; color:#ffffff; font-size:10px; }
.radius-small { border-radius:2px; } .rounding-demo .radius-medium { border-radius:6px; background-color:#8d5d9c; } .rounding-demo .radius-pill { border-radius:18px; background-color:#ba6b26; }
.shadow-demo { display:flex; align-items:center; justify-content:center; background-color:#dce3e7; } .shadow-sheet { padding:14px 18px; border-radius:5px; background-color:#ffffff; box-shadow:7px 8px 12px #20313d55; color:#53616a; font-size:10px; }
.motion-demo { position:relative; overflow:hidden; background-color:#172a34; } .motion-track { height:4px; margin-top:12px; overflow:hidden; border-radius:2px; background-color:#42545f; } .motion-scan { width:24%; height:4px; background-color:#3dd6b3; animation:1.8s cubic-in-out scan-line infinite alternate; } .motion-square { position:absolute; right:16px; bottom:12px; width:18px; height:18px; border:3px #f2b84b; border-radius:3px; animation:5s linear turn infinite; }
@keyframes scan-line { from { transform:translateX(0%); } to { transform:translateX(310%); } } @keyframes turn { from { transform:rotate(0deg); } to { transform:rotate(360deg); } }
.swatches { display:flex; align-items:center; justify-content:space-between; } .swatch { width:24px; height:42px; border-radius:4px; } .teal { background-color:#168c78; } .blue { background-color:#3277a8; } .amber { background-color:#d69a2f; } .coral { background-color:#c85c4b; } .violet { background-color:#76549a; }
.layout-stage { position:relative; display:grid; grid-template-columns:72px minmax(0px,1fr); grid-template-rows:34px 100px 28px; grid-template-areas:"head head" "side main" "foot foot"; gap:5px; padding:9px; border-radius:5px; background-color:#273640; color:#dce6eb; font-family:"JetBrains Mono"; font-size:9px; }
.layout-header { grid-area:head; padding:10px; background-color:#336f83; } .layout-side { grid-area:side; padding:10px; background-color:#674f78; } .layout-main { grid-area:main; padding:10px; background-color:#376354; } .layout-foot { grid-area:foot; padding:7px 10px; background-color:#7d572f; }
.anchor-badge { position:absolute; width:24px; height:20px; padding-top:4px; box-sizing:border-box; border-radius:3px; text-align:center; background-color:#f4c151; color:#29333a; } .anchor-ne { top:15px; right:15px; } .anchor-sw { left:15px; bottom:15px; background-color:#ec7968; }
.material-workspace { display:grid; grid-template-columns:minmax(0px,7fr) minmax(280px,5fr); gap:16px; }
.material-preview { position:relative; display:flex; flex-direction:column; justify-content:flex-end; min-height:170px; overflow:hidden; padding:18px; box-sizing:border-box; border:1px #166c5c; border-radius:6px; background-color:#17313a; decorator:ue-material(showcase.energy); }
.material-preview-copy { position:relative; padding:12px 14px; border-left:3px #f2c14f; background-color:#13232bd9; color:#ffffff; }
.material-preview-copy span,.material-label { color:#9fb2ba; font-family:"JetBrains Mono"; font-size:8px; } .material-preview-copy strong { margin-top:4px; font-size:17px; } .material-preview-copy p { margin:5px 0 0; color:#c6d2d6; font-size:9px; }
.material-meter { position:relative; height:5px; margin-top:10px; overflow:hidden; border-radius:3px; background-color:#102029; } .material-meter span { height:5px; background-color:#f2c14f; }
.material-controls { display:flex; flex-direction:column; justify-content:center; min-width:0; padding:18px; border-left:4px #3277a8; background-color:#edf2f4; }
.material-controls > strong { margin:6px 0 10px; color:#176f75; font-family:"JetBrains Mono"; font-size:24px; } .material-controls p { margin:11px 0 0; color:#61717a; font-size:10px; line-height:1.5; }
.mask-showcase { display:flex; flex:1; flex-direction:column; align-items:center; justify-content:center; min-height:0; background-color:#10171c; color:#eef2f3; text-align:center; }
.mask-showcase h2 { margin:8px 0 4px; color:#f3d373; font-size:30px; }
.mask-showcase p { margin:0; color:#93a3aa; font-size:12px; }
.mask-kicker { color:#c99534; font-family:"JetBrains Mono"; font-size:9px; }
.mask-contract { display:grid; grid-template-columns:70px 130px; gap:8px 16px; margin-top:24px; padding:14px 18px; border-top:1px #654819; border-bottom:1px #654819; text-align:left; }
.mask-contract span { color:#8d9ba1; font-family:"JetBrains Mono"; font-size:9px; }
.mask-contract strong { color:#f1d47c; font-size:11px; }
.mask-probe { margin-top:20px; padding:9px 14px; border:1px #c08a2f; border-radius:4px; background-color:#241e14; color:#f7dfa0; }
.mask-probe strong { display:inline; margin-left:8px; color:#ffffff; }
.gold-edge-mask { position:fixed; top:0; left:0; width:100%; height:100%; overflow:hidden; z-index:100; pointer-events:none; }
.gold-edge-art,.gold-edge-art svg { position:absolute; top:0; left:0; width:100%; height:100%; pointer-events:none; }
.gold-flow { position:absolute; overflow:hidden; pointer-events:none; }
.gold-flow span { position:absolute; display:block; border-radius:8px; }
.gold-flow-top { top:5px; left:4%; width:92%; height:28px; } .gold-flow-top span { left:0; width:18%; }
.gold-flow-bottom { bottom:5px; left:4%; width:92%; height:28px; } .gold-flow-bottom span { left:0; width:22%; }
.gold-flow-left { top:5%; left:5px; width:28px; height:90%; } .gold-flow-left span { top:0; height:22%; }
.gold-flow-right { top:5%; right:5px; width:28px; height:90%; } .gold-flow-right span { top:0; height:18%; }
.gold-flow-top .flow-aura,.gold-flow-bottom .flow-aura { top:0; height:24px; background-color:#bd690b; opacity:0.18; }
.gold-flow-top .flow-glow,.gold-flow-bottom .flow-glow { top:6px; height:13px; background-color:#ffc32e; opacity:0.46; }
.gold-flow-top .flow-core,.gold-flow-bottom .flow-core { top:11px; height:4px; background-color:#fffce0; opacity:0.98; }
.gold-flow-left .flow-aura,.gold-flow-right .flow-aura { left:0; width:24px; background-color:#bd690b; opacity:0.18; }
.gold-flow-left .flow-glow,.gold-flow-right .flow-glow { left:6px; width:13px; background-color:#ffc32e; opacity:0.46; }
.gold-flow-left .flow-core,.gold-flow-right .flow-core { left:11px; width:4px; background-color:#fffce0; opacity:0.98; }
.gold-spark { position:absolute; width:11px; height:11px; border-radius:11px; background-color:#e99110; opacity:0.68; pointer-events:none; }
.gold-spark span { width:5px; height:5px; margin:3px; border-radius:5px; background-color:#fffbd6; }
.spark-a { top:34px; left:12%; animation:1.7s 0.1s gold-spark-down infinite; } .spark-b { top:42px; left:38%; animation:1.9s 0.7s gold-spark-down infinite; }
.spark-c { top:30px; left:74%; animation:2.2s 1.2s gold-spark-down infinite; } .spark-d { top:24%; right:34px; animation:2.1s 0.4s gold-spark-left infinite; }
.spark-e { top:67%; right:42px; animation:1.8s 1.1s gold-spark-left infinite; } .spark-f { bottom:34px; left:18%; animation:1.9s 0.2s gold-spark-up infinite; }
.spark-g { bottom:40px; left:62%; animation:2.3s 0.9s gold-spark-up infinite; } .spark-h { top:36%; left:34px; animation:2.3s 0.5s gold-spark-right infinite; }
.spark-i { top:72%; left:42px; animation:2s 1.4s gold-spark-right infinite; } .spark-j { bottom:30px; left:86%; animation:1.6s 0.8s gold-spark-up infinite; }
@keyframes gold-spark-down { from { opacity:0; transform:translateY(-8px) scale(0.4); } 32% { opacity:1; transform:translateY(4px) scale(1.25); } to { opacity:0; transform:translateY(34px) scale(0.3); } }
@keyframes gold-spark-up { from { opacity:0; transform:translateY(8px) scale(0.4); } 32% { opacity:1; transform:translateY(-4px) scale(1.25); } to { opacity:0; transform:translateY(-34px) scale(0.3); } }
@keyframes gold-spark-left { from { opacity:0; transform:translateX(8px) scale(0.4); } 32% { opacity:1; transform:translateX(-4px) scale(1.25); } to { opacity:0; transform:translateX(-34px) scale(0.3); } }
@keyframes gold-spark-right { from { opacity:0; transform:translateX(-8px) scale(0.4); } 32% { opacity:1; transform:translateX(4px) scale(1.25); } to { opacity:0; transform:translateX(34px) scale(0.3); } }
.chart-workspace { display:grid; grid-template-columns:minmax(0px,7fr) minmax(310px,5fr); gap:16px; align-items:stretch; }
.chart-surface { min-width:0; height:300px; overflow:hidden; border:1px #d4dade; background-color:#ffffff; }
.echarts-svg-host,.echarts-svg-host svg { display:block; width:100%; height:300px; }
.chart-table { min-width:0; border:1px #cbd4d9; background-color:#ffffff; }
.chart-row { display:grid; grid-template-columns:minmax(72px,2fr) 56px 52px minmax(120px,3fr); gap:8px; align-items:center; min-height:46px; box-sizing:border-box; padding:7px 10px; border-bottom:1px #e1e7ea; color:#45545e; font-size:11px; }
.chart-row:last-child { border-bottom-width:0; }
.chart-header { min-height:34px; background-color:#edf2f3; color:#71808a; font-family:"JetBrains Mono"; font-size:9px; }
.chart-value { color:#087461; font-family:"JetBrains Mono"; font-size:15px; font-weight:bold; } .chart-target { color:#a66c19; font-family:"JetBrains Mono"; }
.chart-slider { display:block; width:100%; height:22px; }
.chart-slider slidertrack { height:6px; margin-top:8px; border-radius:3px; background-color:#d9e3e5; }
.chart-slider sliderprogress { height:6px; border-radius:3px; background-color:#168c78; }
.chart-slider sliderbar { width:12px; height:18px; border:3px #ffffff; border-radius:4px; background-color:#168c78; }
.chart-slider sliderarrowdec,.chart-slider sliderarrowinc { width:0; }
.split-shell { display:flex; flex-direction:column; height:380px; min-height:0; overflow:hidden; border:1px #c5ced4; border-radius:5px; background-color:#ffffff; box-shadow:inset 0 1px 4px #25364018; }
.text-scroll { box-sizing:border-box; flex-shrink:0; overflow:auto; padding:18px 24px; border:0; border-radius:0; background-color:#ffffff; color:#45545e; line-height:1.65; }
.text-scroll h2 { margin:0 0 12px; color:#20313a; font-size:20px; } .text-scroll p { margin:0 0 14px; font-size:13px; } .text-scroll p strong { display:inline; color:#176f75; } .text-scroll .text-lead { padding-left:12px; border-left:3px #d99b32; color:#566772; font-size:14px; }
.split-handle { position:relative; height:13px; flex-shrink:0; box-sizing:border-box; border-top:1px #b7c2c9; border-bottom:1px #b7c2c9; background-color:#dce3e7; cursor:resize; drag:drag; }
.split-handle:hover,.split-handle.active { border-color:#118774; background-color:#bce7dc; }
.split-grip { width:48px; height:3px; margin:4px auto 0; border-radius:2px; background-color:#75858f; }
.split-handle.active .split-grip { background-color:#087461; }
.split-lower { min-height:72px; flex:1 1 0%; overflow:hidden; padding:14px 16px; background-color:#edf2f3; }
.split-lower-heading { display:flex; justify-content:space-between; margin-bottom:10px; color:#74828c; font-family:"JetBrains Mono"; font-size:9px; }
.split-summary { display:grid; grid-template-columns:repeat(3,minmax(0px,1fr)); gap:8px; }
.split-summary div { min-width:0; padding:10px 12px; border-left:3px #168c78; border-radius:3px; background-color:#ffffff; box-shadow:0 2px 6px #25364018; }
.split-summary div:nth-child(2) { border-left-color:#3277a8; } .split-summary div:nth-child(3) { border-left-color:#d69a2f; }
.split-summary strong { overflow:hidden; color:#33434c; font-size:11px; white-space:nowrap; } .summary-label { margin-bottom:4px; color:#7a8992; font-family:"JetBrains Mono"; font-size:8px; }
scrollbarvertical { width:10px; } scrollbarvertical slidertrack { background-color:#e2e8f0; } scrollbarvertical sliderbar { width:6px; min-height:24px; margin:0 2px; background-color:#94a3b8; } scrollbarvertical sliderarrowdec,scrollbarvertical sliderarrowinc { height:0; }
@media (max-width:760px) { .actor-heading .col-span-2,.actor-heading .col-span-1,.actor-row .col-span-2,.actor-row .col-span-1 { display:none; } .actor-heading .col-span-5,.actor-row .col-span-5 { grid-column:span 7 / span 7; } .actor-heading .col-span-4,.actor-row .col-span-4 { grid-column:span 5 / span 5; } .lab-grid { grid-template-columns:minmax(0px,1fr); grid-template-areas:"icons" "image" "effects" "layout" "material" "chart" "text"; } .material-workspace,.chart-workspace { grid-template-columns:minmax(0px,1fr); } .chart-row { grid-template-columns:minmax(68px,2fr) 46px 46px minmax(96px,3fr); } .text-scroll { padding:14px 16px; } .split-summary { grid-template-columns:minmax(0px,1fr); } }
</style>
