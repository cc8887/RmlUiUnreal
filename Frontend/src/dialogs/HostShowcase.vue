<script setup lang="ts">
import { computed, onMounted, ref, type RmlEvent } from '@rmlui/vue';
import { setTheme } from '../platform';
import RmlDialog from './RmlDialog.vue';
import RmlPopover from './RmlPopover.vue';
import RmlMultiSelect from './RmlMultiSelect.vue';

const dark = ref(false);
const outerOpen = ref(false), innerOpen = ref(false), popoverOpen = ref(false);
const backgroundClicks = ref(0);
const name = ref('Explorer'), deferred = ref('Commit on blur');
const quality = ref('balanced'), mode = ref('translate');
const tags = ref(['layout']), flags = ref(new Set(['inspect']));
const imeText = ref(''), composition = ref('Idle'), submitted = ref(0);
const slider = ref(40);
const tagOptions = [{ value: 'layout', label: 'Layout' }, { value: 'input', label: 'Input' }, { value: 'render', label: 'Render' }];
const flagsLabel = computed(() => [...flags.value].sort().join(', ') || 'None');
function applyTheme(): void {
  setTheme(dark.value ? { '--host-surface': '#17252e', '--host-field': '#263a45', '--host-ink': '#eef5f7', '--host-accent': '#ffca62' } :
    { '--host-surface': '#f4f8fa', '--host-field': '#ffffff', '--host-ink': '#263a45', '--host-accent': '#0b846f' });
}
function toggleTheme(): void { dark.value = !dark.value; applyTheme(); }
function inputKey(event: RmlEvent): void {
  if (event.keyName === 'Enter' && !event.isComposing && composition.value !== 'Composing') { event.preventDefault(); submitted.value++; }
}
onMounted(applyTheme);
</script>

<template>
  <div id="host-showcase" class="host-page">
    <div class="host-heading"><div><h2>Native interaction lab</h2><p>Theme, keyboard controls and overlays that follow the viewport.</p></div><button id="host-theme-toggle" class="host-button" @click="toggleTheme"><span id="host-theme-label">Theme:</span><span id="host-theme-value">{{ dark ? 'Dark' : 'Light' }}</span></button></div>
    <div id="host-theme-probe" class="host-theme-probe">Shared theme colors apply immediately.</div>
    <div class="host-grid">
      <section class="host-card"><h3>Forms and keyboard</h3>
        <label class="host-label" for="host-name-input">Live name</label><input id="host-name-input" v-model.trim="name" class="host-input" type="text" /><strong id="host-name-value">{{ name }}</strong>
        <label class="host-label" for="host-lazy-input">Deferred value</label><input id="host-lazy-input" v-model.lazy.trim="deferred" class="host-input" type="text" /><strong id="host-lazy-value">{{ deferred }}</strong>
        <div class="host-inline"><label><input id="host-mode-translate" v-model="mode" name="host-mode" type="radio" value="translate" /><span id="host-mode-translate-label">Translate</span></label><label><input id="host-mode-rotate" v-model="mode" name="host-mode" type="radio" value="rotate" /><span id="host-mode-rotate-label">Rotate</span></label><strong id="host-mode-value">{{ mode }}</strong></div>
        <select id="host-quality-select" v-model="quality" class="host-input"><option value="fast">Fast</option><option value="balanced">Balanced</option><option value="quality">Quality</option></select><strong id="host-quality-value">{{ quality }}</strong>
        <RmlMultiSelect id="host-tags" v-model="tags" :options="tagOptions" /><strong id="host-tags-value">{{ tags.join(', ') || 'None' }}</strong>
        <div class="host-inline"><label><input id="host-flag-inspect" v-model="flags" type="checkbox" value="inspect" /><span id="host-flag-inspect-label">Inspect</span></label><label><input id="host-flag-edit" v-model="flags" type="checkbox" value="edit" /><span id="host-flag-edit-label">Edit</span></label><strong id="host-flags-value">{{ flagsLabel }}</strong></div>
        <input id="host-keyboard-slider" v-model.number="slider" type="range" min="0" max="100" step="5" class="chart-slider" /><strong id="host-slider-value">{{ slider }}</strong>
      </section>
      <section class="host-card"><h3>Text composition</h3><p>Compose text using your input method. Enter submits only after composition is complete.</p>
        <input id="host-ime-input" v-model="imeText" class="host-input" type="text" @compositionstart="composition = 'Composing'" @compositionend="composition = 'Committed'" @keydown="inputKey" />
        <div class="host-result"><span id="host-ime-status">{{ composition }}</span><strong id="host-ime-value">{{ imeText || 'Waiting for text' }}</strong><span>Submissions: <strong id="host-submit-count">{{ submitted }}</strong></span></div>
        <h3>Nested dialogs</h3><p>Tab stays in the active dialog. Escape closes the top dialog and returns focus.</p><button id="host-outer-trigger" class="host-button" @click="outerOpen = true"><span id="host-outer-trigger-label">Open parent dialog</span></button>
        <button id="host-background-probe" class="host-button" @click="backgroundClicks++"><span id="host-background-label">Background clicks:</span><span id="host-background-count">{{ backgroundClicks }}</span></button>
        <h3>Popover in a scrolling card</h3><div id="host-popover-scroll" class="host-popover-scroll"><div class="host-scroll-before">Scroll this card to move the anchor.</div><button id="host-popover-anchor" class="host-button" @click="popoverOpen = !popoverOpen"><span id="host-popover-anchor-label">Open anchored popover</span></button><div class="host-scroll-after">The floating panel leaves this card's clipping boundary.</div></div>
        <RmlPopover v-if="popoverOpen" popover-id="host-popover" anchor-id="host-popover-anchor" @close="popoverOpen = false"><strong>Anchored panel</strong><p>Position follows scrolling and resizes. It flips to remain visible.</p></RmlPopover>
      </section>
    </div>
    <RmlDialog v-if="outerOpen" dialog-id="host-outer-dialog" title-id="host-outer-title" initial-focus-id="host-outer-first" trigger-id="host-outer-trigger" :close-on-backdrop="false" @close="outerOpen = false; innerOpen = false">
      <div class="modal-header"><h2 id="host-outer-title">Parent dialog</h2></div><div class="modal-body"><input id="host-outer-first" class="modal-input" type="text" value="First focus target" /><button id="host-inner-trigger" class="host-button" @click="innerOpen = true"><span id="host-inner-trigger-label">Open child dialog</span></button></div><div class="modal-actions"><button id="host-outer-close" class="host-button" @click="outerOpen = false"><span id="host-outer-close-label">Close parent</span></button></div>
      <RmlDialog v-if="innerOpen" dialog-id="host-inner-dialog" title-id="host-inner-title" initial-focus-id="host-inner-first" trigger-id="host-inner-trigger" @close="innerOpen = false"><div class="modal-header"><h2 id="host-inner-title">Child dialog</h2></div><div class="modal-body"><input id="host-inner-first" class="modal-input" type="text" value="Child focus target" /></div><div class="modal-actions"><button id="host-inner-close" class="host-button" @click="innerOpen = false"><span id="host-inner-close-label">Return to parent</span></button></div></RmlDialog>
    </RmlDialog>
  </div>
</template>

<style>
.host-page { display:flex; flex-direction:column; flex:1; min-height:0; overflow:auto; padding:20px; background-color:var(--host-surface,#f4f8fa); color:var(--host-ink,#263a45); }
.host-heading { display:flex; justify-content:space-between; align-items:center; gap:16px; }
.host-heading h2 { margin:0; font-size:21px; } .host-heading p { margin:5px 0 0; font-size:11px; }
.host-theme-probe { margin:15px 0; padding:10px 14px; border-left:4px var(--host-accent,#0b846f); color:var(--host-accent,#0b846f); background-color:var(--host-field,#ffffff); font-size:12px; }
.host-grid { display:grid; grid-template-columns:minmax(0px,1fr) minmax(0px,1fr); gap:18px; }
.host-card { padding:18px; background-color:var(--host-field,#ffffff); border:1px #aabcc7; border-radius:5px; }
.host-card h3 { margin:0 0 12px; font-size:15px; } .host-card h3:not(:first-child) { margin-top:24px; }
.host-card p { margin:0 0 12px; font-size:11px; line-height:1.5; }
.host-card strong { margin:5px 0 9px; font-size:11px; color:var(--host-accent,#0b846f); }
.host-label { display:block; margin:12px 0 5px; font-size:10px; }
.host-input { display:block; width:100%; height:34px; box-sizing:border-box; padding:6px 9px; border:1px #9cafba; border-radius:3px; background-color:var(--host-field,#ffffff); color:var(--host-ink,#263a45); }
.host-inline { display:flex; align-items:center; flex-wrap:wrap; gap:12px; margin:12px 0; font-size:11px; } .host-inline label { display:flex; align-items:center; gap:6px; }
.host-inline input { width:18px; height:18px; flex-shrink:0; box-sizing:border-box; border:2px #78959f; border-radius:3px; background-color:#ffffff; }
.host-inline input[type=radio] { border-radius:9px; }
.host-inline input:checked { border:5px #0b846f; }
.host-inline input:focus { border-color:#df9b22; background-color:#fff1d0; }
.host-button { display:flex; align-items:center; gap:6px; padding:8px 12px; margin:6px 0; border:1px #168c78; border-radius:4px; background-color:#eaf5f1; color:#174f43; font-size:11px; }
.host-button:focus,.host-input:focus { border:2px #df9b22; }
.host-result { margin:12px 0 18px; padding:12px; border-left:3px #168c78; font-size:11px; }
.host-popover-scroll { position:relative; height:142px; overflow:auto; border:1px #93aab7; padding:10px; }
.host-scroll-before { height:22px; font-size:10px; } .host-scroll-after { height:160px; padding-top:18px; font-size:10px; }
@media (max-width:760px) { .host-grid { grid-template-columns:minmax(0px,1fr); } .host-heading { align-items:flex-start; flex-direction:column; } }
</style>
