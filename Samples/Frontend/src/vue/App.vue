<script setup lang="ts">
import { computed, ref, onMounted, onUnmounted } from '@rmlui/vue';
import { findService } from '../../../../Frontend/src/bridge';
import { state, version, addProject, reverseProjects, removeProject, save } from './store';
import ProjectCard from './ProjectCard.vue';
const total = computed(() => state.projects.length);
const hostName = ref('Unreal Engine');
const probeResult = ref('No C++ object');
interface DemoProbeObject { GetIdentity(): string; Add(left: number, right: number): number }
interface DemoHostService { GetHostName(): string; GetProbeObject(): DemoProbeObject }
const host = findService<DemoHostService>('host');
let timer = 0;
onMounted(() => {
  timer = setInterval(() => state.ticks++, 1000) as unknown as number;
  if (host) {
    hostName.value = host.GetHostName();
    const probe = host.GetProbeObject();
    probeResult.value = `${probe.GetIdentity()}:${probe.Add(19, 23)}`;
  }
});
onUnmounted(() => clearInterval(timer));
</script>

<template>
  <div id="vue-workspace" :class="state.accent">
    <div id="vue-header">
      <div><h1>Vue / RmlUi</h1><p class="muted">Northstar workspace</p></div>
      <span id="vue-status" class="status">{{ state.status }}</span>
    </div>
    <div id="vue-sidebar">
      <h2>Session settings</h2>
      <label for="vue-name">Session name</label>
      <input id="vue-name" type="text" v-model="state.name" />
      <label for="vue-accent">Accent</label>
      <select id="vue-accent" v-model="state.accent">
        <option value="teal">Evergreen</option><option value="blue">Ocean</option><option value="rose">Rose</option>
      </select>
      <label for="vue-intensity">Intensity</label>
      <input id="vue-intensity" class="range" type="range" min="0" max="100" step="1" v-model.number="state.intensity" />
      <div class="check"><input id="vue-enabled" type="checkbox" v-model="state.enabled" /><label for="vue-enabled">Active session</label></div>
      <button id="vue-save" class="primary" :disabled="state.busy" @click="save">Save session</button>
      <div class="sidebar-info"><p class="muted">COMPLETED SAVES</p><p id="vue-saves" class="number">{{ state.saves }}</p></div>
    </div>
    <div id="vue-main">
      <div class="section-title"><h2 id="vue-preview-name">{{ state.name }}</h2><span id="vue-active" class="status">{{ state.enabled ? 'Connected' : 'Paused' }}</span></div>
      <div id="vue-metrics">
        <div><span class="muted">Projects</span><p id="vue-count" class="number">{{ total }}</p></div>
        <div><span class="muted">Intensity</span><p id="vue-percent" class="number">{{ state.intensity }}%</p></div>
        <div><span class="muted">Session time</span><p id="vue-ticks" class="number">{{ state.ticks }}s</p></div>
      </div>
      <div class="section-title"><h2>Activity</h2><div class="commands"><button id="vue-reverse" @click="reverseProjects">Reverse order</button><button id="vue-add" @click="addProject">Add project</button></div></div>
      <div id="vue-projects">
        <ProjectCard v-for="project in state.projects" :key="project.id" :id="'vue-project-' + project.id" :project="project" @remove="removeProject" />
      </div>
      <div id="vue-asset"><img src="hello_world.png" /><div><h3>Interface reference</h3><p class="muted">RmlUi sample asset</p></div></div>
    </div>
    <div id="vue-footer"><span id="vue-host" class="muted">{{ hostName }}</span><span id="vue-probe" class="muted">{{ probeResult }}</span><span id="vue-version" class="muted">{{ version }}</span></div>
  </div>
</template>

<style>
body { margin:0; padding:28px; width:100%; height:100%; box-sizing:border-box; background-color:#f6f8f8; color:#26383c; font-family:LatoLatin; font-size:15px; overflow:auto; }
div,h1,h2,h3,p { display:block; } h1 { font-size:26px; margin:0 0 5px; } h2 { font-size:18px; margin:0; } h3 { font-size:15px; margin:0; } p { margin:5px 0; }
.muted { color:#708184; font-size:13px; } .status { color:#008a7b; font-size:14px; }
#vue-workspace { display:grid; grid-template-columns:240px minmax(0px,1fr); grid-template-areas:"header header" "sidebar main" "footer footer"; gap:26px; }
#vue-header { grid-area:header; display:grid; grid-template-columns:1fr auto; align-items:center; border-bottom:1px #d9e3e2; padding-bottom:20px; }
#vue-sidebar { grid-area:sidebar; border-right:1px #d9e3e2; padding-right:26px; }
#vue-main { grid-area:main; min-width:0; }
#vue-footer { grid-area:footer; display:grid; grid-template-columns:1fr auto auto; gap:18px; border-top:1px #d9e3e2; padding-top:16px; }
label { display:block; margin:20px 0 8px; color:#506669; font-size:14px; }
input[type=text],select { display:block; width:100%; height:38px; box-sizing:border-box; padding:8px 10px; background-color:#ffffff; border:1px #c9d7d5; color:#26383c; border-radius:4px; }
select selectvalue { width:100%; } select selectarrow { width:16px; background-color:#e1ece8; }
select selectbox { background-color:#ffffff; border:1px #b8cfca; padding:4px; }
select option { display:block; padding:8px; } select option:hover,select option:checked { background-color:#e0f1e9; }
button { display:block; padding:9px 13px; background-color:#ffffff; border:1px #c9d7d5; color:#365354; border-radius:4px; text-align:center; font-size:13px; }
button:hover { background-color:#e9f1ee; } button:active { background-color:#d2e6dd; } button:disabled { opacity:0.5; }
.primary { width:100%; box-sizing:border-box; background-color:#008a7b; border-color:#008a7b; color:#ffffff; margin-top:24px; }
.primary:hover { background-color:#007668; } .primary:active { background-color:#005f55; }
.check { display:flex; align-items:center; gap:10px; margin-top:22px; } .check label { margin:0; }
input[type=checkbox] { width:14px; height:14px; border:1px #afc4bf; background-color:#ffffff; border-radius:2px; }
input[type=checkbox]:checked { border:4px #008a7b; }
input.range { display:block; width:100%; height:22px; }
input.range slidertrack { height:6px; margin-top:8px; background-color:#dce8e4; border-radius:3px; }
input.range sliderprogress { height:6px; margin-top:0; background-color:#008a7b; border-radius:3px; }
input.range sliderbar { width:10px; height:18px; background-color:#008a7b; border:3px #f6f8f8; border-radius:3px; }
input.range sliderarrowdec,input.range sliderarrowinc { width:0; }
.sidebar-info { margin-top:30px; padding-top:18px; border-top:1px #d9e3e2; }
.number { font-size:29px; margin-top:8px; }
.section-title { display:grid; grid-template-columns:1fr auto; align-items:center; gap:12px; margin-bottom:18px; }
.commands { display:flex; gap:8px; }
#vue-metrics { display:grid; grid-template-columns:repeat(3,minmax(0px,1fr)); gap:20px; margin-bottom:30px; padding:18px 0; border-top:3px #008a7b; border-bottom:1px #d9e3e2; }
#vue-projects { display:grid; grid-template-columns:repeat(auto-fit,minmax(220px,1fr)); gap:14px; }
.project { background-color:#ffffff; border:1px #d3dfdc; padding:16px; border-radius:5px; min-width:0; }
.project-heading { display:grid; grid-template-columns:1fr auto; gap:8px; align-items:center; margin-bottom:12px; }
.remove { padding:5px 7px; font-size:11px; color:#98605d; }
.track { height:5px; background-color:#e6edeb; margin-top:22px; } .fill { height:5px; background-color:#008a7b; }
.percentage { margin-top:12px; color:#607975; font-size:13px; }
.blue .fill { background-color:#4178b8; } .rose .fill { background-color:#b85e7b; }
#vue-asset { display:grid; grid-template-columns:160px 1fr; gap:18px; align-items:center; margin-top:26px; border-top:1px #d9e3e2; padding-top:20px; }
#vue-asset img { display:block; width:160px; height:auto; }
scrollbarvertical { width:10px; } scrollbarvertical slidertrack { background-color:#edf2f0; } scrollbarvertical sliderbar { width:6px; min-height:24px; margin:0 2px; background-color:#afc4bc; }
scrollbarvertical sliderarrowdec,scrollbarvertical sliderarrowinc { height:0; }
@media (max-width:760px) { body { padding:16px; } #vue-workspace { grid-template-columns:minmax(0px,1fr); grid-template-areas:"header" "sidebar" "main" "footer"; gap:22px; } #vue-sidebar { border-right-width:0; padding-right:0; } .sidebar-info { display:none; } .section-title { grid-template-columns:1fr; } #vue-projects { grid-template-columns:1fr; } #vue-footer { grid-template-columns:1fr; gap:8px; } }
</style>
