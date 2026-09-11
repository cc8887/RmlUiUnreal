<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref } from '@rmlui/vue';
import { getService } from '../bridge';

interface ActorSummary { path: string; name: string; type: string; level: string; hidden: boolean; ticking: boolean }
interface ActorProperty { category: string; name: string; type: string; value: string }
interface ActorDetails { found: boolean; path?: string; name?: string; type?: string; level?: string; transform?: { location: string; rotation: string; scale: string }; properties?: ActorProperty[] }
interface ActorSnapshot { sequence: number; level: string; actors: ActorSummary[] }
interface ActorObserverService { GetActorSnapshot(): string; GetActorDetails(actorPath: string): string }

const observer = getService<ActorObserverService>('actorObserver');
const snapshot = ref<ActorSnapshot>({ sequence: 0, level: '', actors: [] });
const selectedPath = ref('');
const showDetails = ref(false);
const details = ref<ActorDetails | null>(null);
const query = ref('');
const activeView = ref<'actors' | 'lab'>('actors');
const isLive = ref(true);
const error = ref('');
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
onMounted(() => { refresh(); timer = setInterval(() => { if (isLive.value) refresh(); }, 500) as unknown as number; });
onUnmounted(() => clearInterval(timer));
</script>

<template>
  <div class="observer-shell flex h-full w-full flex-col text-ink">
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

    <div v-else id="ui-lab" class="lab-scroll min-h-0 flex-1 overflow-auto">
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
      </div>
    </div>
  </div>
</template>

<style>
@tailwind utilities;
body { margin:0; width:100%; height:100%; overflow:hidden; background-color:#eef1f3; font-family:"Noto Sans CJK SC"; font-size:14px; }
div,h1,p,span,section,strong { display:block; } button,img,input { display:block; }
button { cursor:pointer; font-family:"Noto Sans CJK SC"; } button:disabled { cursor:default; opacity:0.45; }
.observer-shell { background-color:#eef1f3; }
.topbar { border-color:#cbd2d8; background-color:#f9fafb; box-shadow:0 2px 8px #18242e18; }
.live-state { color:#677580; } .live-state.running { color:#087f69; } .live-state.paused { color:#a76314; }
.live-dot { width:7px; height:7px; border-radius:7px; background-color:#9aa6af; } .running .live-dot { background-color:#0d9b7d; animation:1.2s cubic-in-out live-pulse infinite alternate; } .paused .live-dot { background-color:#d18b2c; }
@keyframes live-pulse { from { opacity:0.35; transform:scale(0.8); } to { opacity:1; transform:scale(1.15); } }
.view-tabs { height:42px; background-color:#f4f6f7; border-color:#cbd2d8; }
.view-tab { display:flex; align-items:center; gap:7px; height:42px; padding:0 16px; border:0; border-bottom:3px transparent; border-radius:0; background-color:transparent; color:#64717c; }
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
.lab-grid { display:grid; grid-template-columns:minmax(0px,7fr) minmax(280px,5fr); grid-template-areas:"icons image" "effects layout"; gap:14px; padding:16px; }
.lab-panel { min-width:0; padding:16px; border:1px #c8d0d6; border-radius:6px; background-color:#f9fafb; box-shadow:0 5px 14px #26354120; }
.icon-panel { grid-area:icons; } .image-panel { grid-area:image; } .effects-panel { grid-area:effects; } .layout-panel { grid-area:layout; }
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
scrollbarvertical { width:10px; } scrollbarvertical slidertrack { background-color:#e2e8f0; } scrollbarvertical sliderbar { width:6px; min-height:24px; margin:0 2px; background-color:#94a3b8; } scrollbarvertical sliderarrowdec,scrollbarvertical sliderarrowinc { height:0; }
@media (max-width:760px) { .actor-heading .col-span-2,.actor-heading .col-span-1,.actor-row .col-span-2,.actor-row .col-span-1 { display:none; } .actor-heading .col-span-5,.actor-row .col-span-5 { grid-column:span 7 / span 7; } .actor-heading .col-span-4,.actor-row .col-span-4 { grid-column:span 5 / span 5; } .lab-grid { grid-template-columns:minmax(0px,1fr); grid-template-areas:"icons" "image" "effects" "layout"; } }
</style>
