<script setup lang="ts">
import { ref } from '@rmlui/vue';
import RmlDialog from './RmlDialog.vue';

type DialogKind = 'headless' | 'shadcn' | 'alert' | 'sheet';
const openDialog = ref<DialogKind | null>(null);
const triggerId = ref('');
const activity = ref('Idle');

function open(kind: DialogKind, trigger: string): void {
  triggerId.value = trigger;
  openDialog.value = kind;
}
function close(message?: string): void {
  if (message) activity.value = message;
  openDialog.value = null;
}
</script>

<template>
  <div id="dialog-showcase" class="dialog-page">
    <div class="dialog-heading">
      <div><span class="dialog-eyebrow">TAILWIND PATTERNS / NATIVE RMLUI</span><strong>Overlay component compatibility</strong></div>
      <div class="dialog-result"><span>LAST ACTION</span><strong id="dialog-action-status">{{ activity }}</strong></div>
    </div>

    <div class="dialog-content">
      <section class="dialog-family">
        <div class="family-heading"><div class="family-icon teal"><img src="icons/app-window-white.png" /></div><div><strong>Headless UI Dialog</strong><span>Tailwind Labs interaction contract</span></div><span class="compat-badge">ADAPTED</span></div>
        <p>Centered overlay with explicit title, backdrop dismissal, Escape handling and focus restoration.</p>
        <button id="headless-dialog-trigger" class="dialog-demo-button primary" @click="open('headless', 'headless-dialog-trigger')"><img src="icons/app-window-white.png" /><span>Open Headless dialog</span></button>
      </section>

      <section class="dialog-family shadcn-family">
        <div class="family-heading"><div class="family-icon dark"><img src="icons/panels-top-left-white.png" /></div><div><strong>shadcn/ui overlays</strong><span>Common source-owned component patterns</span></div><span class="compat-badge">3 TESTS</span></div>
        <div class="shadcn-options">
          <div class="option-row"><div><strong>Dialog</strong><span>Modal task surface</span></div><button id="shadcn-dialog-trigger" class="dialog-demo-button" @click="open('shadcn', 'shadcn-dialog-trigger')"><span>Open</span></button></div>
          <div class="option-row"><div><strong>Alert Dialog</strong><span>Explicit destructive confirmation</span></div><button id="shadcn-alert-trigger" class="dialog-demo-button danger-outline" @click="open('alert', 'shadcn-alert-trigger')"><span>Test</span></button></div>
          <div class="option-row"><div><strong>Sheet</strong><span>Right-side complementary panel</span></div><button id="shadcn-sheet-trigger" class="dialog-demo-button" @click="open('sheet', 'shadcn-sheet-trigger')"><span>Open</span></button></div>
        </div>
      </section>
    </div>

    <div class="dialog-foot">
      <span>OVERLAYS</span><strong>Native portal and modal stack</strong><span>KEYBOARD</span><strong>Tab loop / Escape / focus restore</strong>
    </div>

    <RmlDialog v-if="openDialog === 'headless'" dialog-id="headless-dialog" title-id="headless-dialog-title" initial-focus-id="headless-dialog-confirm" :trigger-id="triggerId" @close="close('Headless dialog dismissed')">
      <div class="modal-header"><div><span>HEADLESS UI PATTERN</span><h2 id="headless-dialog-title">Publish interface changes?</h2></div><button id="headless-dialog-close" class="modal-close" title="Close" @click="close('Headless dialog dismissed')"><img src="icons/x.png" /></button></div>
      <div class="modal-body"><p>The new component bundle will become active for mounted Actor Observer views.</p><div class="notice"><strong>Atomic activation</strong><span>The previous version remains available if validation fails.</span></div></div>
      <div class="modal-actions"><button class="modal-button" @click="close('Headless dialog cancelled')"><span>Cancel</span></button><button id="headless-dialog-confirm" class="modal-button primary wide" @click="close('Headless dialog confirmed')"><span>Publish changes</span></button></div>
    </RmlDialog>

    <RmlDialog v-if="openDialog === 'shadcn'" dialog-id="shadcn-dialog" title-id="shadcn-dialog-title" initial-focus-id="shadcn-dialog-save" :trigger-id="triggerId" @close="close('shadcn Dialog dismissed')">
      <div class="modal-header"><div><span>SHADCN / DIALOG</span><h2 id="shadcn-dialog-title">Edit display profile</h2></div><button id="shadcn-dialog-close" class="modal-close" title="Close" @click="close('shadcn Dialog dismissed')"><img src="icons/x.png" /></button></div>
      <div class="modal-body"><label class="field-label">Profile name</label><input class="modal-input" type="text" value="Editor compact" /><label class="field-label">Description</label><input class="modal-input" type="text" value="Dense tools and diagnostics" /></div>
      <div class="modal-actions"><button class="modal-button" @click="close('shadcn Dialog cancelled')"><span>Cancel</span></button><button id="shadcn-dialog-save" class="modal-button dark wide" @click="close('shadcn Dialog saved')"><span>Save changes</span></button></div>
    </RmlDialog>

    <RmlDialog v-if="openDialog === 'alert'" dialog-id="shadcn-alert-dialog" title-id="shadcn-alert-title" initial-focus-id="shadcn-alert-cancel" :trigger-id="triggerId" :close-on-backdrop="false" @close="close('shadcn Alert Dialog dismissed')">
      <div class="alert-layout"><div class="alert-icon"><img src="icons/triangle-alert.png" /></div><div><h2 id="shadcn-alert-title">Remove cached bundle?</h2><p>This removes the selected local version. The currently active bundle is not affected.</p></div></div>
      <div class="modal-actions"><button id="shadcn-alert-cancel" class="modal-button" @click="close('shadcn Alert Dialog cancelled')"><span>Cancel</span></button><button id="shadcn-alert-confirm" class="modal-button danger wide" @click="close('shadcn Alert Dialog confirmed')"><span>Remove bundle</span></button></div>
    </RmlDialog>

    <RmlDialog v-if="openDialog === 'sheet'" dialog-id="shadcn-sheet" title-id="shadcn-sheet-title" initial-focus-id="shadcn-sheet-close" :trigger-id="triggerId" placement="right" @close="close('shadcn Sheet dismissed')">
      <div class="sheet-header"><div class="family-icon blue"><img src="icons/panel-right-open-white.png" /></div><div><span>SHADCN / SHEET</span><h2 id="shadcn-sheet-title">Inspector settings</h2></div><button id="shadcn-sheet-close" class="modal-close" title="Close" @click="close('shadcn Sheet closed')"><img src="icons/x.png" /></button></div>
      <div class="sheet-body"><div class="setting-row"><div><strong>Live updates</strong><span>Refresh the selected object automatically</span></div><span class="setting-value">500 ms</span></div><div class="setting-row"><div><strong>Property scope</strong><span>Editable and Blueprint-visible fields</span></div><span class="setting-value">Default</span></div><div class="setting-row"><div><strong>Layout density</strong><span>Compact rows for editor workflows</span></div><span class="setting-value">Compact</span></div></div>
      <div class="sheet-footer"><button class="modal-button dark wide" @click="close('shadcn Sheet applied')"><span>Apply settings</span></button></div>
    </RmlDialog>
  </div>
</template>

<style>
.dialog-page { position:relative; display:flex; flex-direction:column; min-height:0; flex:1; background-color:#e9eef0; }
.dialog-heading { display:flex; align-items:center; justify-content:space-between; flex-shrink:0; min-height:66px; padding:10px 20px; border-bottom:1px #c6d0d5; background-color:#f9fafb; }
.dialog-heading strong { margin-top:4px; color:#25343c; font-size:15px; } .dialog-eyebrow { color:#687985; font-family:"JetBrains Mono"; font-size:9px; }
.dialog-result { min-width:220px; padding:8px 12px; border-left:3px #168c78; background-color:#edf5f3; } .dialog-result span { color:#6d7d86; font-family:"JetBrains Mono"; font-size:8px; } .dialog-result strong { overflow:hidden; margin-top:3px; font-size:11px; white-space:nowrap; }
.dialog-content { display:grid; grid-template-columns:minmax(260px,5fr) minmax(360px,7fr); min-height:0; flex:1; gap:14px; padding:18px; }
.dialog-family { min-width:0; padding:18px; border:1px #c5cfd5; border-radius:6px; background-color:#ffffff; box-shadow:0 5px 14px #2635411e; }
.family-heading { display:flex; align-items:center; gap:11px; padding-bottom:14px; border-bottom:1px #e0e5e8; } .family-heading > div:nth-child(2) { min-width:0; flex:1; }
.family-heading strong { color:#293940; font-size:14px; } .family-heading span { margin-top:3px; color:#71808a; font-size:10px; }
.family-icon { display:flex; align-items:center; justify-content:center; width:38px; height:38px; border-radius:5px; background-color:#168c78; } .family-icon.dark { background-color:#26343c; } .family-icon.blue { background-color:#3277a8; } .family-icon img { width:20px; height:20px; }
.family-heading .compat-badge { flex-shrink:0; margin:0; padding:5px 7px; border:1px #bad8d1; border-radius:3px; background-color:#e9f6f2; color:#087461; font-family:"JetBrains Mono"; font-size:8px; }
.dialog-family > p { margin:18px 0; color:#5e6d76; font-size:12px; line-height:1.55; }
.dialog-demo-button,.modal-button { display:flex; align-items:center; justify-content:center; gap:7px; height:34px; box-sizing:border-box; padding:0 12px; border:1px #b9c5cb; border-radius:4px; background-color:#ffffff; color:#35454e; font-size:11px; }
.dialog-demo-button:hover,.modal-button:hover { border-color:#168c78; background-color:#edf8f5; } .dialog-demo-button img { width:15px; height:15px; }
.dialog-demo-button.primary,.modal-button.primary { border-color:#0c806b; background-color:#0c806b; color:#ffffff; } .modal-button.dark { border-color:#26343c; background-color:#26343c; color:#ffffff; }
.modal-button { min-width:82px; } .modal-button.wide { min-width:126px; }
.shadcn-options { margin-top:10px; } .option-row { display:flex; align-items:center; justify-content:space-between; min-height:58px; padding:8px 2px; border-bottom:1px #e1e7ea; } .option-row:last-child { border-bottom-width:0; }
.option-row strong { color:#35444c; font-size:12px; } .option-row span { margin-top:3px; color:#77858e; font-size:9px; } .option-row .dialog-demo-button { min-width:72px; }
.dialog-demo-button.danger-outline { border-color:#d7a29a; color:#a64035; } .modal-button.danger { border-color:#b9473a; background-color:#b9473a; color:#ffffff; }
.dialog-foot { display:grid; grid-template-columns:78px minmax(0px,1fr) 82px minmax(0px,1fr); flex-shrink:0; gap:10px; min-height:52px; box-sizing:border-box; padding:12px 18px; border-top:1px #c6d0d5; background-color:#f8fafb; }
.dialog-foot span { color:#73828b; font-family:"JetBrains Mono"; font-size:8px; } .dialog-foot strong { overflow:hidden; color:#3c4d56; font-size:10px; white-space:nowrap; }
.modal-header,.sheet-header { display:flex; align-items:flex-start; justify-content:space-between; padding:18px 20px 14px; border-bottom:1px #dbe2e6; }
.modal-header span,.sheet-header span { color:#71808a; font-family:"JetBrains Mono"; font-size:8px; } .modal-header h2,.sheet-header h2,.alert-layout h2 { margin:5px 0 0; color:#23333b; font-size:17px; }
.modal-close { display:flex; align-items:center; justify-content:center; width:30px; height:30px; padding:0; border:1px #c5ced3; border-radius:4px; background-color:#ffffff; } .modal-close img { width:15px; height:15px; }
.modal-body { padding:18px 20px; } .modal-body p { margin:0 0 14px; color:#53636d; font-size:12px; line-height:1.55; }
.notice { padding:12px; border-left:3px #168c78; background-color:#edf6f3; } .notice strong { color:#315148; font-size:11px; } .notice span { margin-top:4px; color:#65756f; font-size:9px; }
.modal-actions { display:flex; justify-content:flex-end; gap:8px; padding:13px 20px; border-top:1px #dbe2e6; background-color:#f7f9fa; }
.field-label { display:block; margin:0 0 6px; color:#586872; font-size:10px; font-weight:bold; } .modal-input { display:block; width:100%; height:35px; box-sizing:border-box; margin-bottom:14px; padding:7px 9px; border:1px #bdc8ce; border-radius:4px; background-color:#ffffff; color:#26343c; }
.alert-layout { display:grid; grid-template-columns:42px minmax(0px,1fr); gap:13px; padding:22px 20px; } .alert-layout p { margin:8px 0 0; color:#5f6e77; font-size:11px; line-height:1.5; }
.alert-icon { display:flex; align-items:center; justify-content:center; width:38px; height:38px; border-radius:5px; background-color:#fff0ed; } .alert-icon img { width:21px; height:21px; }
.sheet-header { align-items:center; gap:10px; } .sheet-header > div:nth-child(2) { min-width:0; flex:1; }
.sheet-body { min-height:0; flex:1; padding:8px 20px; } .rml-dialog-panel.right { display:flex; flex-direction:column; }
.setting-row { display:flex; align-items:center; justify-content:space-between; min-height:72px; border-bottom:1px #e0e6e9; } .setting-row strong { color:#35454e; font-size:12px; } .setting-row span { margin-top:4px; color:#75838c; font-size:9px; }
.setting-row .setting-value { flex-shrink:0; margin:0 0 0 12px; padding:5px 7px; border-radius:3px; background-color:#e8eef1; color:#445761; font-family:"JetBrains Mono"; font-size:8px; }
.sheet-footer { display:flex; justify-content:flex-end; padding:14px 20px; border-top:1px #dbe2e6; }
@media (max-width:760px) { .dialog-heading { align-items:flex-start; flex-direction:column; gap:8px; } .dialog-result { width:100%; box-sizing:border-box; } .dialog-content { grid-template-columns:minmax(0px,1fr); overflow:auto; } .dialog-foot { grid-template-columns:70px minmax(0px,1fr); } }
</style>
