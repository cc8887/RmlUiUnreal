<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref } from '@rmlui/vue';
import { adaptAnimationJs, type RmlAnimationGroup } from '../../../../Frontend/src/animation-adapters';

interface SpringMetric { id: string; label: string; value: number; detail: string; color: string }

const metrics: SpringMetric[] = [
  { id: 'layout', label: 'Layout', value: 82, detail: 'Stable geometry', color: 'teal' },
  { id: 'render', label: 'Render', value: 68, detail: 'Slate draw pass', color: 'blue' },
  { id: 'bridge', label: 'Bridge', value: 76, detail: 'Typed service calls', color: 'amber' },
  { id: 'input', label: 'Input', value: 54, detail: 'Native event route', color: 'coral' },
];

const revealTargets = [
  '#animation-spring-kicker',
  '#animation-spring-title',
  '#animation-spring-copy',
  ...metrics.map(metric => `#animation-spring-item-${metric.id}`),
  '#animation-spring-summary',
  '#animation-spring-contract',
  '#animation-spring-footer',
];

const state = ref<'ready' | 'running' | 'settled' | 'interrupted' | 'error'>('ready');
const errorMessage = ref('');
let activeGroup: RmlAnimationGroup | undefined;

const stateLabel = computed(() => {
  if (state.value === 'running') return 'RUNNING / NATIVE';
  if (state.value === 'settled') return 'SETTLED / READY';
  if (state.value === 'interrupted') return 'INTERRUPTED';
  if (state.value === 'error') return 'ERROR';
  return 'READY TO PLAY';
});

function replay(): void {
  activeGroup?.cancel();
  errorMessage.value = '';
  state.value = 'running';
  try {
    const group = adaptAnimationJs({
      el: revealTargets,
      draw: { opacity: [0, 1], translateY: [34, -2] },
      dur: 780,
      ease: 'cubic-bezier(0.16,1.32,0.3,1)',
      defer: 0,
      stagger: { each: 0.082 },
    });
    activeGroup = group;
    void group.finished.then(results => {
      if (activeGroup !== group) return;
      state.value = results.every(result => result.reason === 'completed') ? 'settled' : 'interrupted';
    });
  } catch (error) {
    state.value = 'error';
    errorMessage.value = error instanceof Error ? error.message : String(error);
  }
}

onMounted(replay);
onUnmounted(() => {
  activeGroup?.cancel();
  activeGroup = undefined;
});
</script>

<template>
  <div id="animation-spring-showcase" class="animation-spring-page">
    <header class="animation-spring-heading">
      <div class="animation-spring-heading-copy">
        <span class="animation-spring-eyebrow">ANIMATION.JS / NATIVE IR</span>
        <strong>Staggered spring reveal</strong>
        <p>Every layer enters on its own beat while the native track keeps the motion smooth.</p>
      </div>
      <div class="animation-spring-controls">
        <div class="animation-spring-state"><span>STATUS</span><strong id="animation-spring-status">{{ stateLabel }}</strong></div>
        <button id="animation-spring-replay" class="animation-spring-replay" title="Replay spring reveal" @click="replay">
          <img src="icons/rotate-ccw.png" /><span>Replay</span>
        </button>
      </div>
    </header>

    <div class="animation-spring-content">
      <section class="animation-spring-stage-panel">
        <div class="animation-spring-stage">
          <div class="animation-spring-grid-lines animation-spring-grid-lines-a"></div>
          <div class="animation-spring-grid-lines animation-spring-grid-lines-b"></div>
          <span id="animation-spring-kicker" class="animation-spring-kicker animation-spring-target">SEQUENCED REVEAL / 10 TARGETS</span>
          <h2 id="animation-spring-title" class="animation-spring-title animation-spring-target">Motion lands in layers.</h2>
          <p id="animation-spring-copy" class="animation-spring-copy animation-spring-target">A compact Animation.js style draw object is compiled into native opacity and translate tracks. The slight overshoot gives each surface a spring-like arrival without a per-frame JavaScript callback.</p>

          <div class="animation-spring-metrics">
            <article v-for="metric in metrics" :id="'animation-spring-item-' + metric.id" :key="metric.id" class="animation-spring-card animation-spring-target">
              <div class="animation-spring-card-top"><span>{{ metric.label }}</span><strong>{{ metric.value }}%</strong></div>
              <div class="animation-spring-meter"><span :class="'meter-' + metric.color" :style="{ width: `${metric.value}%` }"></span></div>
              <p>{{ metric.detail }}</p>
            </article>
          </div>

          <div id="animation-spring-summary" class="animation-spring-summary animation-spring-target">
            <span class="animation-spring-summary-dot"></span>
            <div><span>PIPELINE</span><strong>HostSnapshot → compiled plan → native batch</strong></div>
            <span class="animation-spring-summary-tail">1 BATCH</span>
          </div>
        </div>
      </section>

      <aside id="animation-spring-contract" class="animation-spring-contract animation-spring-target">
        <div class="animation-spring-contract-heading"><span>IMPLEMENTATION CONTRACT</span><strong>Animation.js mapping</strong></div>
        <div class="animation-spring-contract-list">
          <div><span>DRAW</span><strong>opacity + translateY</strong></div>
          <div><span>EASE</span><strong>cubic overshoot</strong></div>
          <div><span>DURATION</span><strong>780 ms</strong></div>
          <div><span>STAGGER</span><strong>82 ms / target</strong></div>
          <div><span>ROUTE</span><strong>native MovieScene tick</strong></div>
        </div>
        <div class="animation-spring-note"><span>SPRING FEEL</span><p>Overshoot stays in the scalar IR, so the demo keeps the same batched runtime path used by production animations.</p></div>
        <p v-if="errorMessage" id="animation-spring-error" class="animation-spring-error">{{ errorMessage }}</p>
      </aside>
    </div>

    <footer id="animation-spring-footer" class="animation-spring-footer animation-spring-target">
      <span>REFERENCE</span><strong>@olton/animation draw / defer</strong>
      <span>TRANSPORT</span><strong>compiled native plan batch</strong>
      <span>CONTROL</span><strong>replayable group</strong>
    </footer>
  </div>
</template>

<style>
.animation-spring-page { display:flex; flex:1; min-height:0; flex-direction:column; overflow:auto; background-color:#e8edef; color:#202b31; }
.animation-spring-heading { display:flex; align-items:center; justify-content:space-between; flex-shrink:0; min-height:92px; box-sizing:border-box; gap:18px; padding:16px 22px; border-bottom:1px #c9d3d7; background-color:#f8fafb; }
.animation-spring-heading-copy { min-width:0; }
.animation-spring-eyebrow,.animation-spring-state span,.animation-spring-contract-heading span,.animation-spring-summary span,.animation-spring-footer span { color:#6b7b84; font-family:"JetBrains Mono"; font-size:9px; }
.animation-spring-heading strong { display:block; margin-top:5px; color:#26363e; font-size:17px; }
.animation-spring-heading p { margin:6px 0 0; color:#63737b; font-size:10px; }
.animation-spring-controls { display:flex; align-items:center; flex-shrink:0; gap:10px; }
.animation-spring-state { min-width:142px; padding:8px 11px; border-left:3px #168c78; background-color:#edf6f3; }
.animation-spring-state strong { margin-top:4px; color:#087461; font-family:"JetBrains Mono"; font-size:10px; white-space:nowrap; }
.animation-spring-replay { display:flex; align-items:center; gap:7px; min-height:34px; padding:0 11px; border:1px #0d806b; border-radius:4px; background-color:#14866d; color:#f1faf7; font-size:10px; }
.animation-spring-replay:hover,.animation-spring-replay:focus { background-color:#1ba885; }
.animation-spring-replay img { width:15px; height:15px; }
.animation-spring-content { display:grid; grid-template-columns:minmax(0px,7fr) minmax(250px,3fr); gap:14px; min-height:0; flex:1; padding:18px; }
.animation-spring-stage-panel,.animation-spring-contract { min-width:0; border:1px #c5d0d4; border-radius:6px; background-color:#ffffff; }
.animation-spring-stage-panel { display:flex; min-height:390px; padding:14px; }
.animation-spring-stage { position:relative; display:flex; flex:1; min-height:0; flex-direction:column; overflow:hidden; padding:24px; border:1px #294853; background-color:#172b33; }
.animation-spring-grid-lines { position:absolute; left:0; width:100%; height:1px; background-color:#315561; opacity:0.75; }
.animation-spring-grid-lines-a { top:30%; } .animation-spring-grid-lines-b { bottom:23%; }
.animation-spring-kicker { position:relative; color:#65dbc0; font-family:"JetBrains Mono"; font-size:9px; }
.animation-spring-title { position:relative; max-width:620px; margin:9px 0 0; color:#f2f7f6; font-size:26px; line-height:1.12; }
.animation-spring-copy { position:relative; max-width:660px; margin:10px 0 22px; color:#a6bdc1; font-size:11px; line-height:1.55; }
.animation-spring-metrics { position:relative; display:grid; grid-template-columns:repeat(4,minmax(0px,1fr)); gap:9px; margin-top:auto; }
.animation-spring-card { min-width:0; padding:12px; border:1px #3b626c; border-radius:4px; background-color:#203c45; }
.animation-spring-card-top { display:flex; align-items:center; justify-content:space-between; gap:6px; color:#c1d1d2; font-family:"JetBrains Mono"; font-size:9px; }
.animation-spring-card-top strong { color:#f3cb6b; font-size:13px; }
.animation-spring-meter { height:5px; margin-top:12px; overflow:hidden; border-radius:3px; background-color:#11252c; }
.animation-spring-meter span { display:block; height:5px; }
.meter-teal { background-color:#40d6b4; } .meter-blue { background-color:#63a8d0; } .meter-amber { background-color:#efbe5e; } .meter-coral { background-color:#e77b6e; }
.animation-spring-card p { overflow:hidden; margin:9px 0 0; color:#86a2a8; font-size:9px; white-space:nowrap; text-overflow:ellipsis; }
.animation-spring-summary { display:flex; align-items:center; gap:9px; margin-top:12px; padding:10px 12px; border-left:3px #f0bd54; background-color:#203c45; }
.animation-spring-summary-dot { width:7px; height:7px; flex-shrink:0; border-radius:7px; background-color:#f0bd54; }
.animation-spring-summary div { min-width:0; flex:1; }
.animation-spring-summary div span { color:#87a2a8; font-size:8px; }
.animation-spring-summary strong { display:block; overflow:hidden; margin-top:4px; color:#e9f1ef; font-size:10px; white-space:nowrap; text-overflow:ellipsis; }
.animation-spring-summary .animation-spring-summary-tail { flex-shrink:0; color:#65dbc0; font-size:8px; }
.animation-spring-contract { display:flex; flex-direction:column; padding:18px; }
.animation-spring-contract-heading { padding-bottom:12px; border-bottom:1px #dce4e7; }
.animation-spring-contract-heading strong { display:block; margin-top:5px; color:#293a42; font-size:14px; }
.animation-spring-contract-list { margin-top:10px; }
.animation-spring-contract-list div { display:flex; align-items:center; justify-content:space-between; gap:12px; min-height:48px; border-bottom:1px #e0e7e9; }
.animation-spring-contract-list span { color:#77878e; font-family:"JetBrains Mono"; font-size:8px; }
.animation-spring-contract-list strong { color:#354850; font-family:"JetBrains Mono"; font-size:9px; text-align:right; }
.animation-spring-note { margin-top:auto; padding:13px; border-left:3px #3277a8; background-color:#edf2f4; }
.animation-spring-note span { color:#60717a; font-family:"JetBrains Mono"; font-size:8px; }
.animation-spring-note p { margin:7px 0 0; color:#52656e; font-size:10px; line-height:1.5; }
.animation-spring-error { margin:12px 0 0; padding:8px; border-left:3px #c85c4b; background-color:#fff0ed; color:#9b3e32; font-family:"JetBrains Mono"; font-size:8px; line-height:1.4; }
.animation-spring-footer { display:grid; grid-template-columns:62px minmax(0px,1fr) 66px minmax(0px,1fr) 54px minmax(0px,1fr); flex-shrink:0; gap:9px; min-height:48px; box-sizing:border-box; padding:11px 18px; border-top:1px #c8d2d6; background-color:#f8fafb; }
.animation-spring-footer strong { overflow:hidden; color:#40515a; font-size:9px; white-space:nowrap; text-overflow:ellipsis; }
@media (max-width:760px) {
  .animation-spring-heading { align-items:flex-start; flex-direction:column; gap:10px; }
  .animation-spring-controls { width:100%; justify-content:space-between; }
  .animation-spring-content { grid-template-columns:minmax(0px,1fr); overflow:auto; }
  .animation-spring-stage-panel { min-height:470px; }
  .animation-spring-metrics { grid-template-columns:repeat(2,minmax(0px,1fr)); }
  .animation-spring-footer { grid-template-columns:62px minmax(0px,1fr); }
}
</style>
