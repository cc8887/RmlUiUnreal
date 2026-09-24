<script setup lang="ts">
import { onMounted, onUnmounted, ref } from '@rmlui/vue';
import { native } from '../bridge';

type RoutedSupport = 'webcompat' | 'adapter';
type RenderSupport = 'full' | 'degraded';

interface Case {
  prop: string;
  value: string;
  route?: RoutedSupport;
  render?: RenderSupport;
}
interface Result extends Case { ok: boolean; message: string; }

// Standard CSS properties, one probe each. The value is a representative legal
// CSS value so a rejection means "property not supported", not "bad value".
const CASES: Case[] = [
  { prop: 'animation', value: '1s cubic-in-out probe-fade infinite alternate' },
  { prop: 'animation-name', value: 'probe-fade', route: 'webcompat' },
  { prop: 'animation-duration', value: '1s', route: 'webcompat' },
  { prop: 'animation-delay', value: '0.3s', route: 'webcompat' },
  { prop: 'animation-iteration-count', value: 'infinite', route: 'webcompat' },
  { prop: 'animation-timing-function', value: 'cubic-in-out', route: 'webcompat' },
  { prop: 'animation-fill-mode', value: 'both' },
  { prop: 'animation-direction', value: 'alternate', route: 'webcompat' },
  { prop: 'transition', value: 'opacity 0.3s linear' },
  { prop: 'transition-property', value: 'opacity', route: 'webcompat' },
  { prop: 'transition-duration', value: '0.3s', route: 'webcompat' },
  { prop: 'transition-delay', value: '0.1s', route: 'webcompat' },
  { prop: 'transition-timing-function', value: 'linear', route: 'webcompat' },
  { prop: 'transform', value: 'scale(1.1)' },
  { prop: 'transform-origin', value: '50% 50%' },
  { prop: 'rotate', value: '30deg', route: 'adapter' },
  { prop: 'scale', value: '1.2', route: 'adapter' },
  { prop: 'translate', value: '2px 2px' },
  { prop: 'perspective', value: '800px', render: 'degraded' },
  { prop: 'box-shadow', value: '0px 2px 6px #00000088', render: 'degraded' },
  { prop: 'text-shadow', value: '0px 1px 2px #00000088' },
  { prop: 'filter', value: 'blur(2px)', render: 'degraded' },
  { prop: 'backdrop-filter', value: 'blur(2px)', render: 'degraded' },
  { prop: 'opacity', value: '0.6' },
  { prop: 'border-radius', value: '6px' },
  { prop: 'border-top-left-radius', value: '4px' },
  { prop: 'outline', value: '1px #ff0000' },
  { prop: 'outline-width', value: '1px' },
  { prop: 'outline-color', value: '#ff0000' },
  { prop: 'overflow', value: 'hidden' },
  { prop: 'overflow-x', value: 'auto' },
  { prop: 'overflow-y', value: 'auto' },
  { prop: 'clip', value: 'auto' },
  { prop: 'clip-path', value: 'inset(2px)' },
  { prop: 'visibility', value: 'visible' },
  { prop: 'box-sizing', value: 'border-box' },
  { prop: 'aspect-ratio', value: '1' },
  { prop: 'float', value: 'left' },
  { prop: 'clear', value: 'both' },
  { prop: 'vertical-align', value: 'middle' },
  { prop: 'cursor', value: 'pointer' },
  { prop: 'pointer-events', value: 'none' },
  { prop: 'user-select', value: 'none' },
  { prop: 'display', value: 'flex' },
  { prop: 'flex-direction', value: 'column' },
  { prop: 'flex-wrap', value: 'wrap' },
  { prop: 'flex-grow', value: '1' },
  { prop: 'flex-shrink', value: '0' },
  { prop: 'flex-basis', value: '40px' },
  { prop: 'flex', value: '1 1 auto' },
  { prop: 'align-items', value: 'center' },
  { prop: 'align-self', value: 'center' },
  { prop: 'align-content', value: 'center' },
  { prop: 'justify-content', value: 'center' },
  { prop: 'gap', value: '8px' },
  { prop: 'row-gap', value: '8px' },
  { prop: 'column-gap', value: '8px' },
  { prop: 'order', value: '1' },
  { prop: 'grid-template-columns', value: 'repeat(2, minmax(0px, 1fr))' },
  { prop: 'grid-template-rows', value: 'auto auto' },
  { prop: 'grid-auto-flow', value: 'row' },
  { prop: 'grid-auto-columns', value: 'auto' },
  { prop: 'grid-column', value: '1 / 2' },
  { prop: 'grid-row', value: '1 / 2' },
  { prop: 'grid-area', value: 'auto' },
  { prop: 'justify-items', value: 'center' },
  { prop: 'justify-self', value: 'center' },
  { prop: 'place-items', value: 'center' },
  { prop: 'position', value: 'absolute' },
  { prop: 'top', value: '2px' },
  { prop: 'right', value: '2px' },
  { prop: 'bottom', value: '2px' },
  { prop: 'left', value: '2px' },
  { prop: 'z-index', value: '5' },
  { prop: 'inset', value: '2px' },
  { prop: 'width', value: '40px' },
  { prop: 'min-width', value: '10px' },
  { prop: 'max-width', value: '200px' },
  { prop: 'height', value: '20px' },
  { prop: 'min-height', value: '10px' },
  { prop: 'max-height', value: '200px' },
  { prop: 'margin', value: '2px' },
  { prop: 'margin-top', value: '2px' },
  { prop: 'padding', value: '2px' },
  { prop: 'padding-left', value: '2px' },
  { prop: 'color', value: '#ff8800' },
  { prop: 'background-color', value: '#112233' },
  { prop: 'background', value: '#223344' },
  { prop: 'background-image', value: 'none' },
  { prop: 'background-position', value: 'center' },
  { prop: 'background-size', value: 'cover' },
  { prop: 'background-repeat', value: 'no-repeat' },
  { prop: 'font-size', value: '12px' },
  { prop: 'font-family', value: 'Arial' },
  { prop: 'font-weight', value: 'bold' },
  { prop: 'font-style', value: 'italic' },
  { prop: 'font-variant', value: 'small-caps' },
  { prop: 'line-height', value: '1.4' },
  { prop: 'letter-spacing', value: '1px' },
  { prop: 'word-spacing', value: '2px' },
  { prop: 'text-align', value: 'center' },
  { prop: 'text-decoration', value: 'underline' },
  { prop: 'text-transform', value: 'uppercase' },
  { prop: 'text-indent', value: '8px' },
  { prop: 'text-overflow', value: 'ellipsis' },
  { prop: 'white-space', value: 'nowrap' },
  { prop: 'word-break', value: 'break-all' },
  { prop: 'direction', value: 'ltr' },
  { prop: 'border', value: '1px #ff0000' },
  { prop: 'border-width', value: '2px' },
  { prop: 'border-color', value: '#00ff00' },
  { prop: 'border-style', value: 'dashed' },
  { prop: 'border-top', value: '1px #0000ff' },
  { prop: 'border-spacing', value: '2px' },
  { prop: 'fill-image', value: 'icons/list-tree.png' },
  { prop: 'image-color', value: '#ffffff' },
  { prop: 'mix-blend-mode', value: 'normal' },
  { prop: 'caret-color', value: '#ff0000' },
  { prop: 'scroll-behavior', value: 'smooth' },
  { prop: 'scrollbar-width', value: 'thin' },
  { prop: 'overscroll-behavior', value: 'contain' },
  { prop: 'content', value: 'x' },
  { prop: 'list-style', value: 'none' },
  { prop: 'table-layout', value: 'fixed' },
  { prop: 'border-collapse', value: 'collapse' },
  { prop: 'quotes', value: 'none' },
  { prop: 'resize', value: 'none' },
  { prop: 'will-change', value: 'transform' },
  { prop: 'contain', value: 'layout' },
  { prop: 'isolation', value: 'isolate' },
  { prop: 'appearance', value: 'none' },
  { prop: 'all', value: 'unset' },
  { prop: 'object-fit', value: 'cover' },
  { prop: 'object-position', value: 'center' },
];

const TOTAL = CASES.length;
const results = ref<Result[]>([]);
const passed = ref(0);
const routed = ref(0);
const unsupported = ref(0);
const degraded = ref(0);
const running = ref(false);
const done = ref(false);
const activeProp = ref('');
let probeTimer = 0;

function probeAll(): void {
  if (running.value) return;
  running.value = true;
  done.value = false;
  results.value = [];
  passed.value = 0;
  routed.value = 0;
  unsupported.value = 0;
  degraded.value = 0;
  // The runtime rejects a whole page if an unsupported property is set during
  // Activate()'s first layout. afterLayout() still runs inside that window, so
  // defer the sweep until activation has fully completed. Afterwards, rejected
  // properties only log, they no longer roll the page back.
  probeTimer = setTimeout(runSweep, 600) as unknown as number;
}

function runSweep(): void {
  const target = native.FindNode('css-probe-target');
  if (!target) { running.value = false; return; }
  const collected: Result[] = [];
  for (const item of CASES) {
    activeProp.value = item.prop;
    let ok = false;
    let message = '';
    try {
      ok = native.SetProperty(target, item.prop, item.value, false);
      if (!ok) message = native.LastError || 'rejected';
    } catch (error) {
      ok = false;
      message = error instanceof Error ? error.message : String(error);
    }
    if (ok) {
      passed.value += 1;
      if (item.render === 'degraded') degraded.value += 1;
    } else {
      if (item.route) routed.value += 1; else unsupported.value += 1;
    }
    collected.push({ ...item, ok, message });
  }
  results.value = collected;
  activeProp.value = '';
  running.value = false;
  done.value = true;
}

onMounted(() => { probeAll(); });
onUnmounted(() => { if (probeTimer) clearTimeout(probeTimer); });
</script>

<template>
  <div id="css-probe" class="probe-page">
    <div class="probe-head">
      <div>
        <span class="probe-eyebrow">RCSS CAPABILITY PROBE / RUNTIME</span>
        <strong>Standard CSS property sweep</strong>
      </div>
      <div class="probe-head-actions">
        <span id="probe-summary" class="probe-summary">{{ passed }} native / {{ routed }} routed / {{ unsupported }} unsupported</span>
        <button id="probe-rerun" class="probe-button" @click="probeAll">Re-run</button>
      </div>
    </div>

    <div class="probe-body">
      <div id="probe-list" class="probe-list">
        <div v-for="item in results" :key="item.prop" class="probe-row" :class="item.ok ? (item.render === 'degraded' ? 'degraded' : 'ok') : (item.route ? 'routed' : 'bad')">
          <span class="probe-mark">{{ item.ok ? (item.render === 'degraded' ? 'LIMIT' : 'NATIVE') : item.route === 'webcompat' ? 'COMPILE' : item.route === 'adapter' ? 'ADAPTER' : 'NO' }}</span>
          <span class="probe-prop">{{ item.prop }}</span>
          <span class="probe-value">{{ item.value }}</span>
        </div>
      </div>

      <div class="probe-side">
        <div class="probe-panel">
          <strong>Routed support</strong>
          <div v-for="item in results.filter(entry => !entry.ok && entry.route)" :key="'r' + item.prop" class="probe-route">
            <span class="probe-route-prop">{{ item.prop }}</span>
            <span class="probe-route-kind">{{ item.route === 'webcompat' ? 'CSS compiler' : 'animation adapter' }}</span>
          </div>
          <p v-if="done && routed === 0" class="probe-note">No compatibility routes are required.</p>
        </div>
        <div class="probe-panel">
          <strong>Unsupported</strong>
          <div v-for="item in results.filter(entry => !entry.ok && !entry.route)" :key="'f' + item.prop" class="probe-fail">
            <span class="probe-fail-prop">{{ item.prop }}</span>
          </div>
          <p v-if="done && unsupported === 0" class="probe-note">Every property has a supported route.</p>
          <p v-if="!done" class="probe-note">{{ running ? 'probing ' + activeProp : 'waiting for layout…' }}</p>
        </div>
        <div class="probe-panel">
          <strong>Status</strong>
          <p class="probe-note">Cases: {{ TOTAL }}</p>
          <p class="probe-note">Native accepted: {{ passed }}</p>
          <p class="probe-note">Compatibility routes: {{ routed }}</p>
          <p class="probe-note">Slate-RHI limited: {{ degraded }}</p>
          <p class="probe-note">State: {{ running ? 'probing' : done ? 'finished' : 'idle' }}</p>
        </div>
      </div>
    </div>

    <div id="css-probe-target" class="probe-target"></div>
  </div>
</template>

<style>
@keyframes probe-fade { from { opacity: 0.2; } to { opacity: 1; } }
.probe-page { display: flex; flex-direction: column; height: 100%; background-color: #0b141b; color: #cfe0e8; font-size: 11px; }
.probe-head { display: flex; align-items: center; justify-content: space-between; gap: 12px; padding: 12px 16px; border-bottom: 1px #1e3442; background-color: #0e1c26; }
.probe-eyebrow { display: block; color: #5d7c8b; font-size: 9px; }
.probe-head-actions { display: flex; align-items: center; gap: 10px; }
.probe-summary { color: #3dd6b3; font-family: "JetBrains Mono"; font-size: 11px; }
.probe-button { height: 28px; padding: 0 10px; border: 1px #2c4a5b; border-radius: 4px; background-color: #132735; color: #b9cfda; font-size: 10px; }
.probe-body { display: grid; grid-template-columns: minmax(0px, 1fr) 300px; min-height: 0; flex: 1; }
.probe-list { overflow: auto; padding: 10px 14px; }
.probe-row { display: flex; align-items: center; gap: 8px; padding: 3px 6px; border-bottom: 1px #132735; font-family: "JetBrains Mono"; font-size: 10px; }
.probe-row.ok { color: #7fbf9f; }
.probe-row.routed { color: #d7b96b; background-color: #221f13; }
.probe-row.degraded { color: #d99a66; background-color: #251b14; }
.probe-row.bad { color: #e08a7a; background-color: #25151a; }
.probe-mark { width: 54px; }
.probe-prop { width: 200px; }
.probe-value { color: #7f9fae; }
.probe-side { display: flex; flex-direction: column; gap: 12px; padding: 14px 12px; border-left: 1px #1b3140; background-color: #0c1922; overflow: auto; }
.probe-panel { border: 1px #1e3b4a; border-radius: 4px; padding: 8px 10px; background-color: #0f212c; }
.probe-fail { padding: 3px 0; border-bottom: 1px #16303d; }
.probe-fail-prop { color: #e08a7a; font-family: "JetBrains Mono"; font-size: 10px; }
.probe-route { display: flex; justify-content: space-between; gap: 8px; padding: 3px 0; border-bottom: 1px #16303d; }
.probe-route-prop { color: #d7b96b; font-family: "JetBrains Mono"; font-size: 10px; }
.probe-route-kind { color: #6f91a0; font-size: 9px; }
.probe-note { margin: 4px 0 0; color: #648a9c; font-size: 9px; }
.probe-target { width: 4px; height: 4px; }
</style>
