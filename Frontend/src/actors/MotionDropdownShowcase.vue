<script setup lang="ts">
import { computed, onUnmounted, ref } from '@rmlui/vue';

interface MotionOption { id: string; label: string; code: string }

const options: MotionOption[] = [
  { id: 'resize', label: 'Card resize', code: '01' },
  { id: 'dropdown', label: 'Menu dropdown', code: '05' },
  { id: 'reveal', label: 'Panel reveal', code: '07' },
];

const open = ref(false);
const closing = ref(false);
const selected = ref(options[1].label);
let closeTimer = 0;

const state = computed(() => open.value ? 'Open' : closing.value ? 'Closing' : 'Closed');

function clearCloseTimer(): void {
  if (closeTimer) {
    clearTimeout(closeTimer);
    closeTimer = 0;
  }
}

function toggleMenu(): void {
  if (open.value) {
    open.value = false;
    closing.value = true;
    clearCloseTimer();
    closeTimer = setTimeout(() => { closing.value = false; closeTimer = 0; }, 170) as unknown as number;
    return;
  }
  clearCloseTimer();
  closing.value = false;
  open.value = true;
}

function choose(option: MotionOption): void {
  selected.value = option.label;
  if (open.value) toggleMenu();
}

onUnmounted(clearCloseTimer);
</script>

<template>
  <div id="motion-menu-showcase" class="motion-menu-page">
    <div class="motion-menu-heading">
      <div>
        <span class="motion-menu-eyebrow">TRANSITIONS.DEV / RMLUI ADAPTATION</span>
        <strong>Origin-aware dropdown motion</strong>
      </div>
      <div class="motion-menu-state"><span>STATE</span><strong id="motion-menu-state">{{ state }}</strong></div>
    </div>

    <div class="motion-menu-content">
      <section class="motion-menu-stage-panel">
        <div class="motion-panel-heading">
          <div><span>INTERACTIVE SPECIMEN</span><strong>Menu dropdown</strong></div>
          <span class="motion-badge">CONTROLLED STATE</span>
        </div>
        <p class="motion-menu-copy">A compact RmlUi adaptation of the transitions.dev menu-dropdown pattern. Open the menu to sample the scale and opacity transition, then choose an item to exercise the close phase.</p>
        <div id="motion-menu-stage" class="motion-menu-stage">
          <div class="motion-stage-line motion-stage-line-top"></div>
          <div class="motion-stage-line motion-stage-line-bottom"></div>
          <div class="motion-stage-sweep motion-stage-sweep-a"></div>
          <div class="motion-stage-sweep motion-stage-sweep-b"></div>
          <div class="motion-stage-signal"><span class="motion-stage-signal-dot"></span><strong>LIVE PREVIEW</strong></div>
          <span class="motion-stage-label motion-stage-label-top">TRANSFORM ORIGIN / TOP LEFT</span>
          <span class="motion-stage-label motion-stage-label-bottom">SLATE RENDERER / NATIVE INPUT</span>
          <div class="motion-menu-anchor">
            <button id="motion-menu-trigger" class="motion-trigger" :class="open ? 'active' : ''" @click="toggleMenu">
              <span>Motion menu</span><span class="motion-trigger-end"><span class="t-badge" :data-open="open ? 'true' : 'false'"><span class="t-badge-dot">3</span></span><img src="icons/chevron-down-white.png" /></span>
            </button>
            <div id="motion-menu" class="t-dropdown" :class="open ? 'is-open' : closing ? 'is-closing' : ''" data-origin="top-left">
              <div id="motion-menu-surface" class="dropdown-surface">
                <div class="dropdown-heading"><span>Transitions.dev</span><span class="t-digit-group" :class="open ? 'is-animating' : ''"><span class="t-digit">0</span><span class="t-digit" data-stagger="1">3</span></span></div>
                <button v-for="option in options" :id="'motion-option-' + option.id" :key="option.id" class="dropdown-option" :class="'motion-option-' + option.id" @click="choose(option)">
                  <span>{{ option.label }}</span><span>{{ option.code }}</span>
                </button>
              </div>
            </div>
          </div>
          <div class="motion-stage-readout t-resize" :class="open ? 'is-expanded' : ''"><span>SELECTED</span><strong id="motion-menu-selection">{{ selected }}</strong></div>
        </div>
      </section>

      <section class="motion-contract-panel">
        <div class="motion-panel-heading"><div><span>IMPLEMENTATION CONTRACT</span><strong>RmlUi-compatible motion</strong></div></div>
        <div class="motion-contract-list">
          <div class="motion-contract-row"><span>ORIGIN</span><strong id="motion-menu-origin">top-left</strong></div>
          <div class="motion-contract-row"><span>OPEN</span><strong id="motion-menu-open-duration">0.25s / cubic-out</strong></div>
          <div class="motion-contract-row"><span>CLOSE</span><strong id="motion-menu-close-duration">0.15s / cubic-out</strong></div>
          <div class="motion-contract-row"><span>STATE</span><strong>.is-open / .is-closing</strong></div>
        </div>
        <div class="motion-selection"><span>LAST SELECTION</span><strong>{{ selected }}</strong><p>Menu items update the selected value before the close transition completes.</p></div>
      </section>
    </div>

    <div class="motion-menu-foot"><span>REFERENCE</span><strong>transitions.dev menu-dropdown</strong><span>BRIDGE</span><strong>Vue state + RCSS transition</strong></div>
  </div>
</template>

<style>
.motion-menu-page { display:flex; flex:1; min-height:0; flex-direction:column; overflow:auto; background-color:#e8edef; color:#202a2e; }
.motion-menu-heading { display:flex; align-items:center; justify-content:space-between; flex-shrink:0; min-height:68px; box-sizing:border-box; padding:12px 22px; border-bottom:1px #c8d2d6; background-color:#f8fafb; }
.motion-menu-heading strong { display:block; margin-top:5px; color:#26363e; font-size:16px; } .motion-menu-eyebrow,.motion-panel-heading span,.motion-menu-state span,.motion-menu-foot span { color:#6b7b84; font-family:"JetBrains Mono"; font-size:9px; }
.motion-menu-state { min-width:130px; padding:8px 12px; border-left:3px #168c78; background-color:#edf6f3; } .motion-menu-state strong { margin-top:4px; color:#087461; font-family:"JetBrains Mono"; font-size:12px; }
.motion-menu-content { display:grid; grid-template-columns:minmax(0px,7fr) minmax(260px,5fr); gap:14px; min-height:0; flex:1; padding:18px; }
.motion-menu-stage-panel,.motion-contract-panel { min-width:0; padding:18px; border:1px #c5d0d4; border-radius:6px; background-color:#ffffff; box-shadow:0 5px 14px #2635411e; }
.motion-menu-stage-panel { display:flex; flex-direction:column; } .motion-panel-heading { display:flex; align-items:flex-start; justify-content:space-between; gap:12px; padding-bottom:12px; border-bottom:1px #dce4e7; } .motion-panel-heading > div { min-width:0; } .motion-panel-heading strong { display:block; margin-top:5px; color:#293a42; font-size:14px; }
.motion-panel-heading .motion-badge { flex-shrink:0; margin:0; padding:5px 7px; border:1px #b9d9d1; border-radius:3px; background-color:#eaf6f3; color:#087461; font-size:8px; }
.motion-menu-copy { max-width:700px; margin:14px 0; color:#60717a; font-size:11px; line-height:1.55; }
.motion-menu-stage { position:relative; flex:1; min-height:345px; overflow:hidden; border:1px #294853; background-color:#172b33; }
.motion-stage-line { position:absolute; left:7%; width:86%; height:1px; background-color:#37616b; opacity:0.7; } .motion-stage-line-top { top:22%; } .motion-stage-line-bottom { bottom:21%; }
.motion-stage-sweep { position:absolute; left:-32%; width:34%; height:3px; background-color:#42d8bc; opacity:0.72; animation:3.8s cubic-out motion-stage-sweep infinite; } .motion-stage-sweep-a { top:30%; } .motion-stage-sweep-b { top:72%; width:26%; background-color:#f1bd52; opacity:0.66; animation:4.6s 1.1s cubic-out motion-stage-sweep infinite; }
.motion-stage-signal { position:absolute; top:8%; right:7%; display:flex; align-items:center; gap:7px; color:#b9d9d1; font-family:"JetBrains Mono"; font-size:8px; } .motion-stage-signal-dot { width:6px; height:6px; border-radius:6px; background-color:#f1bd52; animation:1.6s cubic-out motion-signal-pulse infinite alternate; }
.motion-stage-label { position:absolute; color:#78939a; font-family:"JetBrains Mono"; font-size:8px; } .motion-stage-label-top { top:24%; left:7%; } .motion-stage-label-bottom { right:7%; bottom:23%; }
.motion-menu-anchor { position:absolute; top:33%; left:14%; z-index:3; width:260px; }
.motion-trigger { position:relative; display:flex; align-items:center; justify-content:space-between; width:260px; height:52px; box-sizing:border-box; padding:0 14px; border:1px #0c806b; border-radius:4px; background-color:#14866d; color:#f1faf7; font-size:12px; text-align:left; transition:background-color 0.25s cubic-out; }
.motion-trigger:hover,.motion-trigger:focus,.motion-trigger.active { background-color:#1ba885; } .motion-trigger-end { position:relative; display:flex; align-items:center; min-width:24px; } .motion-trigger img { width:17px; height:17px; transition:transform 0.25s cubic-out; } .motion-trigger.active img { transform:rotate(180deg); }
.t-badge { position:absolute; top:-11px; right:-9px; z-index:2; pointer-events:none; animation:0.26s cubic-out motion-badge-slide; } .t-badge-dot { display:flex; align-items:center; justify-content:center; width:19px; height:19px; border:2px #172b33; border-radius:12px; box-sizing:border-box; background-color:#f1bd52; color:#27323a; font-family:"JetBrains Mono"; font-size:8px; font-weight:bold; transform:scale(0.72); opacity:0.72; transition:transform 0.5s cubic-out; } .t-badge[data-open="true"] .t-badge-dot { transform:scale(1.16); opacity:1; }
.t-dropdown { position:absolute; top:60px; left:0; width:260px; z-index:4; transform-origin:0% 0%; transform:scale(0.97); pointer-events:none; transition:transform 0.25s cubic-out; }
.t-dropdown[data-origin="top-right"] { transform-origin:100% 0%; } .t-dropdown[data-origin="bottom-left"] { transform-origin:0% 100%; } .t-dropdown[data-origin="bottom-right"] { transform-origin:100% 100%; }
.t-dropdown.is-open { transform:scale(1); pointer-events:auto; } .t-dropdown.is-closing { transform:scale(0.99); pointer-events:none; transition:transform 0.15s cubic-out; }
.dropdown-surface { width:260px; box-sizing:border-box; padding:8px; border-radius:6px; background-color:#f5f8f7; box-shadow:0 14px 30px #0008; opacity:0; transition:opacity 0.25s cubic-out; }
.t-dropdown.is-open .dropdown-surface { opacity:1; } .t-dropdown.is-closing .dropdown-surface { opacity:0; transition:opacity 0.15s cubic-out; }
.dropdown-heading { display:flex; align-items:center; justify-content:space-between; padding:7px 9px 8px; color:#526267; font-size:11px; font-weight:bold; text-transform:uppercase; } .t-digit-group { display:flex; color:#14866d; font-family:"JetBrains Mono"; font-size:10px; } .t-digit { display:block; opacity:1; transform:translateY(0px) scale(1); } .t-digit-group.is-animating .t-digit { animation:0.5s cubic-out motion-digit-pop; } .t-digit-group.is-animating .t-digit[data-stagger="1"] { animation:0.5s 0.07s cubic-out motion-digit-pop; }
.dropdown-option { display:flex; align-items:center; justify-content:space-between; width:100%; min-height:38px; box-sizing:border-box; padding:9px; border:0; border-radius:3px; background-color:transparent; color:#172327; font-size:11px; text-align:left; }
.t-dropdown.is-open .motion-option-resize { animation:0.32s cubic-out motion-dropdown-item-in; } .t-dropdown.is-open .motion-option-dropdown { animation:0.32s 0.04s cubic-out motion-dropdown-item-in; } .t-dropdown.is-open .motion-option-reveal { animation:0.32s 0.08s cubic-out motion-dropdown-item-in; }
.dropdown-option:hover,.dropdown-option:focus { background-color:#e5f3ee; color:#14866d; } .dropdown-option span:last-child { color:#8b9a9d; font-family:"JetBrains Mono"; font-size:9px; }
.motion-stage-readout { position:absolute; right:7%; bottom:8%; width:180px; min-width:180px; box-sizing:border-box; padding:9px 11px; border-left:3px #f1bd52; background-color:#203c45; } .motion-stage-readout span,.motion-selection span { color:#91a9ae; font-family:"JetBrains Mono"; font-size:8px; } .motion-stage-readout strong { display:block; margin-top:4px; color:#f4d27f; font-size:12px; } .t-resize { transition:width 0.3s cubic-out; } .t-resize.is-expanded { width:230px; }
.motion-contract-panel { display:flex; flex-direction:column; } .motion-contract-list { margin-top:12px; } .motion-contract-row { display:flex; align-items:center; justify-content:space-between; min-height:47px; border-bottom:1px #e0e7e9; } .motion-contract-row span { color:#77878e; font-family:"JetBrains Mono"; font-size:8px; } .motion-contract-row strong { color:#354850; font-family:"JetBrains Mono"; font-size:10px; text-align:right; }
.motion-selection { margin-top:auto; padding:14px; border-left:3px #3277a8; background-color:#edf2f4; } .motion-selection strong { display:block; margin-top:5px; color:#236a84; font-size:14px; } .motion-selection p { margin:8px 0 0; color:#65757d; font-size:10px; line-height:1.5; }
.motion-menu-foot { display:grid; grid-template-columns:72px minmax(0px,1fr) 58px minmax(0px,1fr); flex-shrink:0; gap:10px; min-height:48px; box-sizing:border-box; padding:11px 18px; border-top:1px #c8d2d6; background-color:#f8fafb; } .motion-menu-foot strong { overflow:hidden; color:#40515a; font-size:10px; white-space:nowrap; }
@keyframes motion-stage-sweep { from { opacity:0; transform:translateX(0%); } 18% { opacity:0.55; } 78% { opacity:0.22; } to { opacity:0; transform:translateX(520%); } }
@keyframes motion-signal-pulse { from { opacity:0.35; transform:scale(0.72); } to { opacity:1; transform:scale(1.15); } }
@keyframes motion-badge-slide { from { opacity:0; transform:translate(-7px,9px); } to { opacity:1; transform:translate(0px,0px); } }
@keyframes motion-dropdown-item-in { from { opacity:0; transform:translateY(-5px) scale(0.98); } to { opacity:1; transform:translateY(0px) scale(1); } }
@keyframes motion-digit-pop { from { opacity:0; transform:translateY(7px) scale(0.86); } to { opacity:1; transform:translateY(0px) scale(1); } }
@media (max-width:760px) { .motion-menu-heading { align-items:flex-start; flex-direction:column; gap:9px; } .motion-menu-state { width:100%; box-sizing:border-box; } .motion-menu-content { grid-template-columns:minmax(0px,1fr); overflow:auto; } .motion-menu-stage { min-height:330px; } .motion-menu-foot { grid-template-columns:62px minmax(0px,1fr); } }
</style>
