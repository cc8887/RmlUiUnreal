<script setup lang="ts">
import { ref, type RmlEvent } from '@rmlui/vue';
import { check, native } from '../../../../Frontend/src/bridge';

defineOptions({ name: 'NativeCssAnimationShowcase' });
const running = ref(true);
const replayCount = ref(0);
const controlPaused = ref(false);
const controlGeneration = ref(1);
const controlNotice = ref('READY / ONE ACTIVE INSTANCE');
const lifecycleCounts = ref({ start: 0, iteration: 0, end: 0, cancel: 0 });
const lifecycleLog = ref<string[]>([]);
function replay(): void {
  for (let index = 1; index <= 4; index++) {
    const node = native.QueryNode(native.RootNode(), `#css-motion-item-${index}`);
    check(node);
    check(native.RestartCssAnimation(node));
  }
  replayCount.value++;
}

function controlNode(): number {
  return native.QueryNode(native.RootNode(), '#css-control-orbit');
}

function duplicateClass(): void {
  const node = controlNode();
  check(node);
  check(native.SetNodeClass(node, 'css-control-loop', true));
  controlNotice.value = 'DEDUPLICATED / SAME INSTANCE';
}

function rebindControl(): void {
  const node = controlNode();
  check(node);
  check(native.RestartCssAnimation(node));
  controlGeneration.value++;
  controlNotice.value = 'NEW INSTANCE / CANCEL + START';
}

function recordLifecycle(event: RmlEvent): void {
  const detail = event as RmlEvent & { animationName?: string; iteration?: number };
  const key = event.type.replace('animation', '') as 'start' | 'iteration' | 'end' | 'cancel';
  lifecycleCounts.value = { ...lifecycleCounts.value, [key]: lifecycleCounts.value[key] + 1 };
  const suffix = key === 'iteration' ? ` #${detail.iteration ?? 0}` : '';
  lifecycleLog.value = [`${event.type}${suffix}`, ...lifecycleLog.value].slice(0, 4);
}

</script>

<template>
  <div id="native-css-animation-showcase" class="css-motion-page">
    <header class="css-motion-heading">
      <div>
        <span class="css-motion-eyebrow">CSS ANIMATIONS / MOTION MANIFEST V1</span>
        <h2>Native CSS motion pipeline</h2>
        <p>Standard keyframes are compiled once, bound after document load, and advanced by MovieScene ECS.</p>
      </div>
      <div class="css-motion-actions">
        <div class="css-motion-route"><span>ROUTE</span><strong>RCSS → IR → MOVIESCENE</strong></div>
        <button id="css-motion-replay" class="css-motion-replay" title="Replay native CSS animation" @click="replay">
          <img src="icons/rotate-ccw-white.png" /><span>Replay {{ replayCount }}</span>
        </button>
      </div>
    </header>

    <main class="css-motion-content">
      <section id="css-motion-stage" class="css-motion-stage">
        <div id="css-motion-item-1" class="css-motion-item css-motion-item-1" :class="running ? 'css-motion-run' : ''">
          <span>01</span><strong>Parse</strong><p>@keyframes + longhands</p>
        </div>
        <div id="css-motion-item-2" class="css-motion-item css-motion-item-2" :class="running ? 'css-motion-run' : ''">
          <span>02</span><strong>Normalize</strong><p>typed numeric tracks</p>
        </div>
        <div id="css-motion-item-3" class="css-motion-item css-motion-item-3" :class="running ? 'css-motion-run' : ''">
          <span>03</span><strong>Bind</strong><p>RmlUi element handles</p>
        </div>
        <div id="css-motion-item-4" class="css-motion-item css-motion-item-4" :class="running ? 'css-motion-run' : ''">
          <span>04</span><strong>Advance</strong><p>MovieScene entity systems</p>
        </div>
      </section>

      <aside class="css-motion-contract">
        <span class="css-motion-contract-label">LIVE CONTRACT</span>
        <div><span>DECLARATION</span><strong>animation-* longhands</strong></div>
        <div><span>TRACKS</span><strong>opacity + Transform2D</strong></div>
        <div><span>EASING</span><strong>cubic-bezier</strong></div>
        <div><span>STAGGER</span><strong>350 ms</strong></div>
        <div><span>FRAME WORK</span><strong>zero JavaScript callbacks</strong></div>
      </aside>
    </main>

    <section class="css-control-lab">
      <div class="css-control-visual">
        <div class="css-control-copy">
          <span>CONTROL SEMANTICS</span>
          <strong>Infinite timeline with a negative delay</strong>
          <p>The marker starts midway through its cycle. Pause keeps the MovieScene entity and local time intact.</p>
        </div>
        <div class="css-control-track">
          <div class="css-control-track-line"></div>
          <div id="css-control-orbit" class="css-control-orbit css-control-loop"
            :class="controlPaused ? 'css-control-paused' : ''"
            @animationstart="recordLifecycle" @animationiteration="recordLifecycle"
            @animationend="recordLifecycle" @animationcancel="recordLifecycle"><span></span></div>
        </div>
        <div class="css-control-status">
          <span :class="controlPaused ? 'paused' : 'running'">{{ controlPaused ? 'PAUSED' : 'RUNNING' }}</span>
          <strong id="css-control-generation">GEN {{ controlGeneration }}</strong><em>BOUND</em>
          <small id="css-control-notice">{{ controlNotice }}</small>
        </div>
      </div>

      <div class="css-control-panel">
        <div class="css-control-buttons">
          <button id="css-control-pause" :title="controlPaused ? 'Resume animation' : 'Pause animation'" @click="controlPaused = !controlPaused">
            <img :src="controlPaused ? 'icons/play.png' : 'icons/pause.png'" /><span>{{ controlPaused ? 'Resume' : 'Pause' }}</span>
          </button>
          <button id="css-control-duplicate" title="Add the existing animation class again" @click="duplicateClass">
            <img src="icons/circle-plus.png" /><span>Duplicate add</span>
          </button>
          <button id="css-control-rebind" title="Restart the native animation instance" @click="rebindControl">
            <img src="icons/rotate-ccw.png" /><span>Restart</span>
          </button>
        </div>
        <div class="css-event-grid">
          <div><span>START</span><strong id="css-control-start">{{ lifecycleCounts.start }}</strong></div>
          <div><span>ITERATION</span><strong id="css-control-iteration">{{ lifecycleCounts.iteration }}</strong></div>
          <div><span>END</span><strong id="css-control-end">{{ lifecycleCounts.end }}</strong></div>
          <div><span>CANCEL</span><strong id="css-control-cancel">{{ lifecycleCounts.cancel }}</strong></div>
        </div>
        <div class="css-event-log"><span>BATCHED EVENTS</span><strong>{{ lifecycleLog.join(' · ') || 'waiting for MovieScene' }}</strong></div>
      </div>
    </section>

    <footer class="css-motion-footer">
      <span>MANIFEST</span><strong>motion-manifest.json</strong>
      <span>LIFETIME</span><strong>document-bound session</strong>
      <span>RENDER</span><strong>RmlSlate retained sinks</strong>
    </footer>
  </div>
</template>

<style>
.css-motion-page { display:flex; min-height:0; flex:1; flex-direction:column; overflow:auto; background-color:#edf1f2; color:#1e2b31; }
.css-motion-heading { display:flex; min-height:92px; flex-shrink:0; align-items:center; justify-content:space-between; gap:18px; box-sizing:border-box; padding:16px 22px; border-bottom:1px #c7d1d5; background-color:#fafbfb; }
.css-motion-eyebrow,.css-motion-route span,.css-motion-contract-label,.css-motion-contract div span,.css-motion-footer span { color:#687b83; font-family:"JetBrains Mono"; font-size:9px; }
.css-motion-heading h2 { margin:5px 0 0; color:#24343b; font-size:19px; }
.css-motion-heading p { margin:6px 0 0; color:#65767e; font-size:10px; }
.css-motion-actions { display:flex; align-items:center; gap:10px; }
.css-motion-route { min-width:190px; padding:10px 12px; border-left:3px #11836f; background-color:#e7f2ef; }
.css-motion-route strong { display:block; margin-top:4px; color:#0b705f; font-family:"JetBrains Mono"; font-size:10px; white-space:nowrap; }
.css-motion-replay { display:flex; min-height:36px; align-items:center; gap:7px; padding:0 11px; border:1px #0c7866; border-radius:4px; background-color:#11836f; color:#ffffff; font-size:10px; }
.css-motion-replay:hover,.css-motion-replay:focus { background-color:#15977f; }
.css-motion-replay img { width:15px; height:15px; }
.css-motion-content { display:grid; grid-template-columns:minmax(0px,7fr) minmax(250px,3fr); min-height:0; flex:1; gap:14px; padding:18px; }
.css-motion-stage { display:grid; grid-template-columns:repeat(2,minmax(0px,1fr)); align-content:center; gap:12px; min-width:0; padding:24px; border:1px #294852; background-color:#172c34; overflow:hidden; }
.css-motion-item { min-width:0; padding:18px; border:1px #3d626b; border-radius:5px; background-color:#203d45; }
.css-motion-item span { color:#61d6bd; font-family:"JetBrains Mono"; font-size:9px; }
.css-motion-item strong { display:block; margin-top:7px; color:#f2f7f5; font-size:16px; }
.css-motion-item p { margin:5px 0 0; color:#8fa9ae; font-family:"JetBrains Mono"; font-size:9px; }
.css-motion-contract { display:flex; min-width:0; flex-direction:column; padding:18px; border:1px #c5d0d4; background-color:#ffffff; }
.css-motion-contract-label { padding-bottom:12px; border-bottom:1px #dce4e7; }
.css-motion-contract div { display:flex; min-height:52px; align-items:center; justify-content:space-between; gap:10px; border-bottom:1px #e0e7e9; }
.css-motion-contract div strong { color:#33464e; font-family:"JetBrains Mono"; font-size:9px; text-align:right; }
.css-motion-footer { display:grid; grid-template-columns:66px minmax(0px,1fr) 60px minmax(0px,1fr) 48px minmax(0px,1fr); flex-shrink:0; gap:8px; min-height:48px; box-sizing:border-box; padding:11px 18px; border-top:1px #c8d2d6; background-color:#fafbfb; }
.css-motion-footer strong { overflow:hidden; color:#40525a; font-size:9px; white-space:nowrap; text-overflow:ellipsis; }
.css-control-lab { display:grid; grid-template-columns:minmax(0px,6fr) minmax(300px,4fr); flex-shrink:0; gap:14px; padding:0 18px 18px; }
.css-control-visual { display:grid; grid-template-columns:minmax(190px,1fr) minmax(210px,1.3fr) 92px; min-width:0; align-items:center; gap:16px; padding:15px 18px; border:1px #b9c8cd; background-color:#ffffff; }
.css-control-copy span,.css-control-status span,.css-control-status em,.css-event-grid span,.css-event-log span { color:#687b83; font-family:"JetBrains Mono"; font-size:8px; font-style:normal; }
.css-control-copy strong { display:block; margin-top:5px; color:#2b3d44; font-size:12px; }
.css-control-copy p { margin:5px 0 0; color:#72828a; font-size:9px; }
.css-control-track { position:relative; height:38px; overflow:hidden; border:1px #24454e; background-color:#193139; }
.css-control-track-line { position:absolute; top:18px; right:10px; left:10px; height:1px; background-color:#42616a; }
.css-control-orbit { position:absolute; z-index:1; top:9px; left:10px; width:20px; height:20px; border-radius:10px; background-color:#61d6bd; }
.css-control-orbit span { display:block; width:6px; height:6px; margin:7px; border-radius:3px; background-color:#173038; }
.css-control-status { display:flex; flex-direction:column; gap:4px; text-align:right; }
.css-control-status span.running { color:#11836f; }.css-control-status span.paused { color:#b46a20; }
.css-control-status strong { color:#344950; font-family:"JetBrains Mono"; font-size:10px; }
.css-control-status small { color:#5d737a; font-family:"JetBrains Mono"; font-size:7px; line-height:1.25; }
.css-control-panel { display:flex; min-width:0; flex-direction:column; gap:10px; padding:12px; border:1px #bdcbd0; background-color:#f8fafa; }
.css-control-buttons { display:grid; grid-template-columns:repeat(3,minmax(0px,1fr)); gap:7px; }
.css-control-buttons button { display:flex; min-width:0; height:32px; align-items:center; justify-content:center; gap:5px; padding:0 7px; border:1px #aebfc5; border-radius:3px; background-color:#ffffff; color:#40545c; font-size:8px; }
.css-control-buttons button:hover,.css-control-buttons button:focus { border-color:#168670; background-color:#eaf4f1; }
.css-control-buttons img { width:13px; height:13px; }.css-control-buttons span { white-space:nowrap; }
.css-event-grid { display:grid; grid-template-columns:repeat(4,minmax(0px,1fr)); gap:1px; background-color:#d5dfe2; }
.css-event-grid div { padding:7px; background-color:#ffffff; text-align:center; }.css-event-grid strong { display:block; margin-top:3px; color:#193139; font-family:"JetBrains Mono"; font-size:11px; }
.css-event-log { display:flex; min-width:0; justify-content:space-between; gap:8px; }.css-event-log strong { overflow:hidden; color:#4f646c; font-family:"JetBrains Mono"; font-size:8px; white-space:nowrap; text-overflow:ellipsis; }

.css-motion-run {
  animation-duration:2.8s;
  animation-timing-function:cubic-bezier(0.2,0.78,0.24,1);
  animation-iteration-count:1;
  animation-direction:normal;
  animation-fill-mode:both;
  animation-play-state:running;
}
.css-motion-item-1 { animation-name:css-motion-rise; animation-delay:0.20s; }
.css-motion-item-2 { animation-name:css-motion-rise; animation-delay:0.55s; }
.css-motion-item-3 { animation-name:css-motion-rise; animation-delay:0.90s; }
.css-motion-item-4 { animation-name:css-motion-rise; animation-delay:1.25s; }
@keyframes css-motion-rise {
  0% { opacity:0; transform:translateY(28px) scale(0.93); }
  68% { opacity:1; transform:translateY(-5px) scale(1.025); }
  100% { opacity:1; transform:translateY(0px) scale(1); }
}

.css-control-loop {
  animation-name:css-control-travel;
  animation-duration:1.4s;
  animation-delay:-0.55s;
  animation-iteration-count:infinite;
  animation-direction:alternate;
  animation-fill-mode:both;
  animation-timing-function:cubic-bezier(0.3,0.05,0.2,1);
  animation-play-state:running;
}
.css-control-paused { animation-play-state:paused; }
@keyframes css-control-travel {
  from { opacity:0.38; transform:translateX(0px) scale(0.82); }
  72% { opacity:1; transform:translateX(168px) scale(1.12); }
  to { opacity:0.76; transform:translateX(188px) scale(1); }
}

@media (max-width:760px) {
  .css-motion-heading { align-items:flex-start; flex-direction:column; }
  .css-motion-actions { width:100%; justify-content:space-between; }
  .css-motion-route { width:100%; box-sizing:border-box; }
  .css-motion-content { grid-template-columns:minmax(0px,1fr); overflow:auto; }
  .css-motion-stage { min-height:420px; }
  .css-control-lab { grid-template-columns:minmax(0px,1fr); }
  .css-control-visual { grid-template-columns:minmax(0px,1fr); }
  .css-control-status { text-align:left; }
  .css-motion-footer { grid-template-columns:66px minmax(0px,1fr); }
}
</style>
