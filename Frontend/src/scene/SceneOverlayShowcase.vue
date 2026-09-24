<script setup lang="ts">
import { computed, ref } from '@rmlui/vue';

const opacity = ref(10);
const probeCount = ref(0);
const panelStyle = computed(() => ({ backgroundColor: `rgba(8, 22, 30, ${opacity.value}%)` }));
</script>

<template>
  <div id="scene-overlay-showcase" class="scene-overlay">
    <section id="scene-glass-panel" class="scene-panel scene-panel-left" :style="panelStyle" :data-opacity="opacity">
      <span class="scene-eyebrow">FIELD SURVEY / LIVE</span>
      <h2 id="scene-overlay-title">Atrium perimeter</h2>
      <p class="scene-objective">Hold the observation point while the scan resolves.</p>
      <div class="scene-rule"></div>
      <div class="scene-reading"><span>Range</span><strong>184 m</strong></div>
      <div class="scene-reading"><span>Bearing</span><strong>074.2</strong></div>
      <div class="scene-reading"><span>Signal</span><strong class="scene-good">Stable</strong></div>
      <label class="scene-opacity-label" for="scene-opacity"><span>Panel opacity</span><strong id="scene-opacity-value">{{ opacity }}%</strong></label>
      <input id="scene-opacity" v-model.number="opacity" class="scene-opacity" type="range" min="0" max="85" step="1" value="10" />
    </section>

    <div id="scene-open-window" class="scene-open-window">
      <span class="reticle reticle-top"></span><span class="reticle reticle-right"></span>
      <span class="reticle reticle-bottom"></span><span class="reticle reticle-left"></span>
      <span class="scene-target-label">WORLD ACTOR / SceneCubeActor</span>
    </div>

    <section class="scene-panel scene-panel-right" :style="panelStyle">
      <div class="scene-compass"><span>W</span><span class="active">N</span><span>E</span></div>
      <div class="scene-track"><span class="scene-track-fill"></span></div>
      <div class="scene-reading"><span>Exposure</span><strong>Nominal</strong></div>
      <div class="scene-reading"><span>Renderer</span><strong>RmlUi / Slate</strong></div>
      <button id="scene-probe" class="scene-probe" @click="probeCount++"><span>Mark observation</span><strong id="scene-probe-count">{{ probeCount }}</strong></button>
    </section>

    <div class="scene-footer" :style="panelStyle">
      <div><span>VITALS</span><strong>100</strong></div>
      <div class="scene-footer-wide"><span>LOCAL SCENE COMPOSITE</span><strong>TRANSLUCENT UI ACTIVE</strong></div>
      <div><span>CHANNEL</span><strong>ALPHA</strong></div>
    </div>
  </div>
</template>

<style>
.scene-overlay { position:relative; flex:1; min-height:0; overflow:hidden; color:#f0f7f8; pointer-events:none; }
.scene-panel { position:absolute; z-index:2; box-sizing:border-box; border:1px #76c9c8aa; border-radius:5px; color:#edf7f8; pointer-events:auto; }
.scene-panel-left { top:34px; left:30px; width:286px; padding:20px; border-left:4px #4ed1c1; }
.scene-panel-right { top:34px; right:30px; width:252px; padding:17px; border-right:4px #f3b84a; }
.scene-eyebrow { color:#7ee9d8; font-family:"JetBrains Mono"; font-size:9px; }
.scene-panel h2 { margin:7px 0 5px; color:#ffffff; font-size:22px; }
.scene-objective { margin:0; color:#c4d3d6; font-size:11px; line-height:1.5; }
.scene-rule { height:1px; margin:15px 0 10px; background-color:#6ba09e88; }
.scene-reading { display:flex; align-items:center; justify-content:space-between; min-height:26px; color:#aebfc3; font-family:"JetBrains Mono"; font-size:9px; }
.scene-reading strong { color:#ffffff; font-size:10px; } .scene-reading .scene-good { color:#71e1af; }
.scene-opacity-label { display:flex; justify-content:space-between; margin-top:14px; color:#c8dadd; font-size:10px; }
.scene-opacity-label strong { color:#74e5d7; font-family:"JetBrains Mono"; }
.scene-opacity { width:100%; height:24px; margin-top:4px; }
.scene-opacity slidertrack { height:5px; margin-top:9px; border-radius:3px; background-color:#446168; }
.scene-opacity sliderprogress { height:5px; border-radius:3px; background-color:#42c7b7; }
.scene-opacity sliderbar { width:12px; height:18px; border:2px #e9ffff; border-radius:3px; background-color:#168c78; }
.scene-opacity sliderarrowdec,.scene-opacity sliderarrowinc { width:0; }
.scene-compass { display:flex; justify-content:space-between; margin-bottom:12px; color:#8fa2a7; font-family:"JetBrains Mono"; font-size:10px; }
.scene-compass .active { color:#f4c45f; font-size:15px; }
.scene-track { height:4px; margin:6px 0 14px; background-color:#53666b; }
.scene-track-fill { width:68%; height:4px; background-color:#efb340; }
.scene-probe { display:flex; align-items:center; justify-content:space-between; width:100%; margin-top:13px; padding:9px 10px; border:1px #d59d3b; border-radius:3px; background-color:#191d1ecc; color:#f5d38b; }
.scene-probe strong { color:#ffffff; font-family:"JetBrains Mono"; }
.scene-open-window { position:absolute; top:17%; left:28%; width:44%; height:56%; border:1px #85d9d844; pointer-events:none; }
.reticle { position:absolute; background-color:#b8ffffbb; }
.reticle-top,.reticle-bottom { left:50%; width:1px; height:22px; } .reticle-top { top:-11px; } .reticle-bottom { bottom:-11px; }
.reticle-left,.reticle-right { top:50%; width:22px; height:1px; } .reticle-left { left:-11px; } .reticle-right { right:-11px; }
.scene-target-label { position:absolute; left:50%; bottom:12px; width:190px; margin-left:-95px; color:#c7eeee; font-family:"JetBrains Mono"; font-size:8px; text-align:center; }
.scene-footer { position:absolute; left:30px; right:30px; bottom:24px; z-index:2; display:grid; grid-template-columns:120px minmax(0px,1fr) 120px; gap:16px; padding:12px 16px; border-top:1px #69c1bfaa; border-bottom:1px #69c1bfaa; pointer-events:none; }
.scene-footer div { display:flex; flex-direction:column; } .scene-footer div:last-child { text-align:right; }
.scene-footer span { color:#9fb5b8; font-family:"JetBrains Mono"; font-size:8px; }
.scene-footer strong { margin-top:3px; color:#ffffff; font-family:"JetBrains Mono"; font-size:11px; }
.scene-footer-wide { align-items:center; text-align:center; } .scene-footer-wide strong { color:#6de0d2; }
@media (max-width:760px) { .scene-panel-left { left:16px; width:248px; } .scene-panel-right { top:auto; right:16px; bottom:86px; width:220px; } .scene-footer { left:16px; right:16px; grid-template-columns:88px minmax(0px,1fr) 88px; } }
</style>
