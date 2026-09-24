<script setup lang="ts">
import { onBeforeUnmount, onMounted, ref, Teleport, wrapNode, onNodeEvent, type RmlNode, type RmlEvent } from '@rmlui/vue';
import { native } from '../../../../Frontend/src/bridge';
import { containsNode, ensureOverlayRoot, observeLayout } from '../../../../Frontend/src/platform';
import { positionPopover } from './floatingPlatform';

const props = defineProps<{ popoverId: string; anchorId: string }>();
const emit = defineEmits<{ close: [] }>();
const target = wrapNode(ensureOverlayRoot())!;
const panel = ref<RmlNode | null>(null);
const position = ref({ left: '0px', top: '0px', visibility: 'hidden' });
const placement = ref('bottom-start');
let anchor = 0, generation = 0;
let stopObserving = () => {}, stopClick = () => {};
function keydown(event: RmlEvent): void {
  if (event.keyName === 'Escape' && !event.isComposing) { event.stopImmediatePropagation(); event.preventDefault(); emit('close'); }
}
onMounted(() => {
  anchor = native.FindNode(props.anchorId);
  stopObserving = observeLayout(() => panel.value ? [anchor, panel.value.handle] : [], async snapshot => {
    const current = ++generation;
    if (!panel.value) return;
    const result = await positionPopover(snapshot, anchor, panel.value.handle);
    if (!result || current !== generation || !panel.value) return;
    placement.value = result.placement;
    position.value = { left: `${Math.round(result.x)}px`, top: `${Math.round(result.y)}px`, visibility: result.middlewareData.hide?.referenceHidden ? 'hidden' : 'visible' };
  });
  stopClick = onNodeEvent(wrapNode(native.RootNode())!, 'click', event => {
    if (panel.value && !containsNode(panel.value.handle, event.target.handle) && !containsNode(anchor, event.target.handle)) emit('close');
  }, true);
});
onBeforeUnmount(() => {
  ++generation; stopObserving(); stopClick();
  if (panel.value && containsNode(panel.value.handle, native.ActiveNode()) && native.IsNodeValid(anchor)) native.FocusNode(anchor);
});
</script>

<template>
  <Teleport :to="target"><section ref="panel" :id="popoverId" class="rml-popover" :style="position" :data-placement="placement" role="dialog" @keydown.capture="keydown"><slot /><button :id="popoverId + '-close'" class="dialog-demo-button" @click="emit('close')"><span :id="popoverId + '-close-label'">Close</span></button></section></Teleport>
</template>

<style>
.rml-popover { position:fixed; z-index:120; width:268px; box-sizing:border-box; padding:16px; border:1px #158d79; border-radius:6px; background-color:#ffffff; color:#263840; pointer-events:auto; }
.rml-popover p { margin:8px 0 14px; font-size:11px; line-height:1.5; }
</style>
