<script setup lang="ts">
import { onBeforeUnmount, onMounted, ref, Teleport, wrapNode, type RmlNode, type RmlEvent } from '@rmlui/vue';
import { native } from '../bridge';
import { ensureOverlayRoot } from '../platform';
import { isTopModal, modalDepth, pushModal } from './dialogManager';

const props = withDefaults(defineProps<{
  dialogId: string;
  titleId: string;
  initialFocusId: string;
  triggerId: string;
  placement?: 'center' | 'right';
  closeOnBackdrop?: boolean;
}>(), { placement: 'center', closeOnBackdrop: true });
const emit = defineEmits<{ close: [] }>();

const overlay = ref<RmlNode | null>(null);
const target = wrapNode(ensureOverlayRoot())!;
const depth = modalDepth();
let release = () => {};
function close(): void {
  if (overlay.value && isTopModal(overlay.value.handle)) emit('close');
}
function backdrop(): void { if (props.closeOnBackdrop) close(); }
function keydown(event: RmlEvent): void {
  if (event.keyName !== 'Escape' || event.isComposing || !overlay.value || !isTopModal(overlay.value.handle)) return;
  // Native text inputs stop unknown keydowns at the target. Handle modal Escape
  // during capture, before that control listener can suppress the bubble phase.
  event.stopImmediatePropagation(); event.preventDefault();
  close();
}
onMounted(() => { if (overlay.value) release = pushModal(overlay.value.handle, native.FindNode(props.initialFocusId), native.FindNode(props.triggerId)); });
onBeforeUnmount(() => release());
</script>

<template>
  <Teleport :to="target">
  <div ref="overlay" :id="dialogId + '-overlay'" class="rml-dialog-overlay" :class="placement" :style="{ zIndex: 80 + depth }" @keydown.capture="keydown">
    <button :id="dialogId + '-backdrop'" class="rml-dialog-backdrop" tabindex="-1" title="Close dialog" @click="backdrop"></button>
    <section :id="dialogId" class="rml-dialog-panel" :class="placement" role="dialog" aria-modal="true" :aria-labelledby="titleId" @click.stop>
      <slot />
    </section>
  </div>
  </Teleport>
</template>

<style>
.rml-dialog-overlay { position:absolute; top:0; right:0; bottom:0; left:0; z-index:80; display:flex; align-items:center; justify-content:center; pointer-events:auto; }
.rml-dialog-overlay.right { align-items:stretch; justify-content:flex-end; }
.rml-dialog-backdrop { position:absolute; top:0; right:0; bottom:0; left:0; width:100%; height:100%; padding:0; border:0; border-radius:0; background-color:#17242db8; }
.rml-dialog-panel { position:relative; z-index:81; box-sizing:border-box; width:520px; max-width:92%; max-height:92%; overflow:auto; border:1px #aebbc3; border-radius:6px; background-color:#ffffff; box-shadow:0 18px 42px #10192070; }
.rml-dialog-panel.right { width:410px; max-width:82%; height:100%; max-height:100%; border-top-width:0; border-right-width:0; border-bottom-width:0; border-radius:0; }
@media (max-width:760px) { .rml-dialog-panel { width:94%; max-width:94%; } .rml-dialog-panel.right { width:88%; max-width:88%; } }
</style>
