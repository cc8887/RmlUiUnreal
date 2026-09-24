<script setup lang="ts">
import { computed, onUnmounted, ref } from '@rmlui/vue';
import {
  adaptAnimationJs,
  captureAnimationHostSnapshot,
  RmlAnimationAdapterError,
  type RmlAnimationGroup,
} from '../../../../Frontend/src/animation-adapters';

type CaseState = 'ready' | 'running' | 'passed' | 'unsupported' | 'error';
interface OfficialCase {
  id: 'ball' | 'fade' | 'slide' | 'zoom' | 'swirl';
  label: string;
  source: string;
  state: CaseState;
  route: string;
  detail: string;
}

const cases = ref<OfficialCase[]>([
  { id: 'ball', label: 'README ball', source: 'left + top + rotate', state: 'ready', route: '-', detail: 'READY' },
  { id: 'fade', label: 'Effects.fadeIn', source: 'opacity', state: 'ready', route: '-', detail: 'READY' },
  { id: 'slide', label: 'Effects.slideLeftIn', source: 'left + opacity', state: 'ready', route: '-', detail: 'READY' },
  { id: 'zoom', label: 'Effects.zoomIn', source: 'scale + opacity', state: 'ready', route: '-', detail: 'READY' },
  { id: 'swirl', label: 'Effects.swirlIn', source: 'scale + rotate + opacity', state: 'ready', route: '-', detail: 'READY' },
]);
const selected = ref<OfficialCase['id']>('ball');
const activeGroups: RmlAnimationGroup[] = [];

const current = computed(() => cases.value.find(entry => entry.id === selected.value)!);

function update(id: OfficialCase['id'], values: Partial<OfficialCase>): void {
  const entry = cases.value.find(item => item.id === id);
  if (entry) Object.assign(entry, values);
}

function clearGroups(): void {
  while (activeGroups.length) activeGroups.pop()?.cancel();
}

function selectCase(id: OfficialCase['id']): void {
  if (selected.value === id) return;
  clearGroups();
  selected.value = id;
}

function routeOf(groups: readonly RmlAnimationGroup[]): string {
  const routes = new Set(groups.flatMap(group => group.animations.map(animation => animation.route)));
  return [...routes].join(' + ') || '-';
}

function track(id: OfficialCase['id'], groups: RmlAnimationGroup[]): void {
  activeGroups.push(...groups);
  update(id, { state: 'running', route: routeOf(groups), detail: `${groups.reduce((sum, group) => sum + group.animations.length, 0)} TRACKS` });
  void Promise.all(groups.map(group => group.finished)).then(results => {
    if (!groups.some(group => activeGroups.includes(group))) return;
    const passed = results.flat().every(result => result.reason === 'completed');
    update(id, { state: passed ? 'passed' : 'error', detail: passed ? 'COMPLETED' : 'INTERRUPTED' });
  });
}

function adapterFailure(id: OfficialCase['id'], error: unknown): void {
  if (error instanceof RmlAnimationAdapterError) {
    update(id, { state: 'unsupported', route: 'rejected', detail: error.code.toUpperCase() });
    return;
  }
  update(id, { state: 'error', route: 'error', detail: error instanceof Error ? error.message : String(error) });
}

function ballBounds(): { x: number; y: number } {
  const snapshot = captureAnimationHostSnapshot(
    ['#animation-official-field', '#animation-official-ball'], [], true,
  );
  const fieldNode = snapshot.targetGroups[0][0];
  const ballNode = snapshot.targetGroups[1][0];
  const field = snapshot.nodes.find(node => node.node === fieldNode)?.metrics;
  const ball = snapshot.nodes.find(node => node.node === ballNode)?.metrics;
  if (!field || !ball) throw new Error('Official ball metrics are unavailable');
  return {
    x: Math.max(0, (field.clientWidth || field.width) - (ball.clientWidth || ball.width)),
    y: Math.max(0, (field.clientHeight || field.height) - (ball.clientHeight || ball.height)),
  };
}

function playBall(): void {
  const bounds = ballBounds();
  // Official README uses loop:true. The runtime probe bounds it to two loops.
  const horizontal = adaptAnimationJs({
    el: '#animation-official-ball', draw: { left: [0, bounds.x] }, dur: 2000,
    ease: 'easeOutQuad', loop: 2, dir: 'alternate',
  });
  const rotation = adaptAnimationJs({
    el: '#animation-official-ball', draw: { rotate: [0, 360] }, dur: 1200, loop: 2,
  });
  try {
    adaptAnimationJs({
      el: '#animation-official-ball', draw: { top: [0, bounds.y] }, dur: 2000,
      ease: 'easeOutBounce', loop: 1,
    });
  } catch (error) {
    if (!(error instanceof RmlAnimationAdapterError) || error.code !== 'unsupported_easing') throw error;
  }
  track('ball', [horizontal, rotation]);
  update('ball', { detail: '2 RUNNING / BOUNCE REJECTED' });
}

function playSelected(): void {
  clearGroups();
  const id = selected.value;
  update(id, { state: 'ready', route: '-', detail: 'STARTING' });
  try {
    if (id === 'ball') return playBall();
    if (id === 'fade') return track(id, [adaptAnimationJs({
      el: '#animation-official-effect', draw: { opacity: [0, 1] }, dur: 300, ease: 'linear',
    })]);
    if (id === 'slide') return track(id, [adaptAnimationJs({
      el: '#animation-official-effect', draw: { left: [-180, 0], opacity: [0, 1] }, dur: 300, ease: 'linear',
    })]);
    if (id === 'zoom') return track(id, [adaptAnimationJs({
      el: '#animation-official-effect', draw: { scale: [3, 1], opacity: [0, 1] }, dur: 300, ease: 'linear',
    })]);
    return track(id, [adaptAnimationJs({
      el: '#animation-official-effect', draw: { scale: [3, 1], rotate: [180, 0], opacity: [0, 1] },
      dur: 300, ease: 'linear',
    })]);
  } catch (error) {
    adapterFailure(id, error);
  }
}

onUnmounted(clearGroups);
</script>

<template>
  <div id="animation-official-examples" class="official-page">
    <header class="official-header">
      <div><span>ANIMATION.JS 0.5.0 / UPSTREAM</span><strong>Official compatibility matrix</strong></div>
      <button id="animation-official-replay" class="official-replay" title="Run selected case" @click="playSelected">
        <img src="icons/play.png" /><span>Run</span>
      </button>
    </header>

    <div class="official-body">
      <aside class="official-list">
        <button v-for="entry in cases" :id="'animation-official-case-' + entry.id" :key="entry.id"
          class="official-case" :class="{ active: selected === entry.id }" @click="selectCase(entry.id)">
          <span class="official-case-state" :class="'state-' + entry.state"></span>
          <span class="official-case-copy"><strong>{{ entry.label }}</strong><small>{{ entry.source }}</small></span>
          <span class="official-case-result">{{ entry.detail }}</span>
        </button>
      </aside>

      <main class="official-stage-wrap">
        <div class="official-stage-header">
          <div><span>CASE</span><strong>{{ current.label }}</strong></div>
          <div><span>ROUTE</span><strong id="animation-official-route">{{ current.route }}</strong></div>
          <div><span>STATE</span><strong id="animation-official-state">{{ current.state.toUpperCase() }}</strong></div>
        </div>

        <div v-if="selected === 'ball'" id="animation-official-field" class="official-ball-field">
          <div id="animation-official-ball" class="official-ball"><span></span></div>
          <span class="field-axis field-axis-x">X</span><span class="field-axis field-axis-y">Y</span>
        </div>
        <div v-else class="official-effect-field">
          <div id="animation-official-effect" class="official-effect-card">
            <span>OFFICIAL EFFECT</span><strong>{{ current.label }}</strong><small>{{ current.source }}</small>
          </div>
        </div>

        <footer class="official-contract">
          <span>SOURCE</span><strong>github.com/olton/animation @ d1506c2</strong>
          <span>LICENSE</span><strong>MIT</strong>
          <span>BACKEND</span><strong>MovieScene ECS</strong>
        </footer>
      </main>
    </div>
  </div>
</template>

<style>
.official-page { display:flex; min-height:0; flex:1; flex-direction:column; background-color:#edf1f2; color:#25343b; }
.official-header { display:flex; min-height:76px; box-sizing:border-box; align-items:center; justify-content:space-between; gap:16px; padding:14px 20px; border-bottom:1px #c7d0d4; background-color:#fbfcfc; }
.official-header span,.official-stage-header span,.official-contract span { color:#73828a; font-family:"JetBrains Mono"; font-size:8px; }
.official-header strong { display:block; margin-top:5px; font-size:17px; }
.official-replay { display:flex; min-height:34px; align-items:center; gap:7px; padding:0 12px; border:1px #0d806b; border-radius:4px; background-color:#14866d; color:#fff; font-size:10px; }
.official-replay:hover,.official-replay:focus { background-color:#1aa382; }
.official-replay img { width:14px; height:14px; }
.official-body { display:grid; min-height:0; flex:1; grid-template-columns:300px minmax(0px,1fr); }
.official-list { overflow:auto; padding:12px; border-right:1px #c7d0d4; background-color:#f7f9f9; }
.official-case { display:grid; width:100%; min-height:62px; box-sizing:border-box; grid-template-columns:9px minmax(0px,1fr) auto; align-items:center; gap:10px; margin-bottom:7px; padding:10px; border:1px #d1d9dc; border-radius:4px; background-color:#fff; color:#31434b; text-align:left; }
.official-case:hover,.official-case.active { border-color:#258d7a; background-color:#edf7f4; }
.official-case-state { width:7px; height:7px; border-radius:7px; background-color:#9ba7ac; }
.state-running { background-color:#2e9b80; }.state-passed { background-color:#167d68; }.state-unsupported { background-color:#d08b2f; }.state-error { background-color:#bd5549; }
.official-case-copy { min-width:0; }.official-case-copy strong { display:block; overflow:hidden; font-size:10px; white-space:nowrap; text-overflow:ellipsis; }.official-case-copy small { display:block; margin-top:4px; color:#74838a; font-family:"JetBrains Mono"; font-size:8px; }
.official-case-result { max-width:110px; overflow:hidden; color:#617078; font-family:"JetBrains Mono"; font-size:7px; white-space:nowrap; text-overflow:ellipsis; }
.official-stage-wrap { display:flex; min-width:0; min-height:0; flex-direction:column; padding:16px; }
.official-stage-header { display:grid; grid-template-columns:minmax(0px,1fr) 150px 120px; gap:10px; padding:11px 13px; border:1px #ccd5d8; background-color:#fff; }
.official-stage-header strong { display:block; overflow:hidden; margin-top:4px; color:#34474f; font-family:"JetBrains Mono"; font-size:9px; white-space:nowrap; text-overflow:ellipsis; }
.official-ball-field,.official-effect-field { position:relative; min-height:330px; flex:1; overflow:hidden; border:1px #284852; background-color:#172c34; }
.official-ball-field { margin-top:10px; }
.official-ball { position:absolute; top:0; left:0; display:flex; width:56px; height:56px; align-items:center; justify-content:center; border:3px #f5c35f; border-radius:56px; background-color:#d85f54; }
.official-ball span { width:18px; height:18px; border:3px #fff; border-radius:18px; opacity:.8; }
.field-axis { position:absolute; color:#78939a; font-family:"JetBrains Mono"; font-size:8px; }.field-axis-x { right:8px; bottom:7px; }.field-axis-y { top:8px; left:7px; }
.official-effect-field { display:flex; margin-top:10px; align-items:center; justify-content:center; }
.official-effect-card { position:relative; left:0; display:flex; width:220px; height:132px; flex-direction:column; align-items:center; justify-content:center; border:1px #4e747d; border-radius:5px; background-color:#24444d; color:#eef6f4; }
.official-effect-card span,.official-effect-card small { color:#7fdac5; font-family:"JetBrains Mono"; font-size:8px; }.official-effect-card strong { margin:8px 0; font-size:16px; }.official-effect-card small { color:#9bb1b6; }
.official-contract { display:grid; min-height:44px; box-sizing:border-box; grid-template-columns:52px minmax(0px,1fr) 48px 70px 58px 120px; align-items:center; gap:8px; padding:9px 12px; border:1px #ccd5d8; border-top:0; background-color:#fff; }
.official-contract strong { overflow:hidden; color:#42545c; font-size:8px; white-space:nowrap; text-overflow:ellipsis; }
@media (max-width:760px) { .official-body { grid-template-columns:minmax(0px,1fr); overflow:auto; }.official-list { border-right:0; border-bottom:1px #c7d0d4; }.official-stage-wrap { min-height:440px; }.official-contract { grid-template-columns:52px minmax(0px,1fr); } }
</style>
