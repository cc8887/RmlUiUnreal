<script setup lang="ts">
import { onBeforeUnmount, onMounted, ref, type RmlNode } from '@rmlui/vue';
import { check, native } from '../../../../Frontend/src/bridge';
import { observeLayout, queryNodes } from '../../../../Frontend/src/platform';
import source from '../../assets/gold-edge-mask.svg';

const host = ref<RmlNode | null>(null);
const flows = [
  { side: 'top', axis: 'X', fraction: 0.18, duration: 3.4, reverse: false },
  { side: 'right', axis: 'Y', fraction: 0.18, duration: 3.1, reverse: false },
  { side: 'bottom', axis: 'X', fraction: 0.22, duration: 4.1, reverse: true },
  { side: 'left', axis: 'Y', fraction: 0.22, duration: 3.8, reverse: true },
];
let stopObserving = () => {};
let animated: number[] = [];
let dimensions = '';

onMounted(() => {
  if (!host.value) return;
  check(native.SetInnerRml(host.value.handle, source));
  native.SetAttribute(host.value.handle, 'data-mask-engine', 'lunasvg', false);
  const containers = flows.map(flow => native.FindNode(`gold-flow-${flow.side}`));
  stopObserving = observeLayout(() => containers, snapshot => {
    const sizes = containers.map(handle => snapshot.nodes.find(node => node.handle === handle));
    if (sizes.some(node => !node || node.width <= 0 || node.height <= 0)) return;
    const signature = JSON.stringify(sizes.map(node => [node!.width, node!.height]));
    if (signature === dimensions) return;
    dimensions = signature;
    animated = [];
    flows.forEach((flow, index) => {
      const extent = flow.axis === 'X' ? sizes[index]!.width : sizes[index]!.height;
      const band = extent * flow.fraction;
      const start = flow.reverse ? extent : -band;
      const end = flow.reverse ? -band : extent;
      for (const handle of queryNodes('span', containers[index])) {
        check(native.AnimateNode(handle, 'transform', `translate${flow.axis}(${start}px)`, `translate${flow.axis}(${end}px)`, flow.duration, -1));
        animated.push(handle);
      }
    });
    if (host.value) native.SetAttribute(host.value.handle, 'data-flow-layout', String(snapshot.revision), false);
  });
});
onBeforeUnmount(() => {
  stopObserving();
  for (const handle of animated) if (native.IsNodeValid(handle)) native.CancelAnimation(handle, 'transform');
  if (host.value) native.SetInnerRml(host.value.handle, '');
});
</script>

<template>
  <div id="gold-edge-mask" class="gold-edge-mask">
    <div ref="host" class="gold-edge-art"></div>
    <div v-for="flow in flows" :id="'gold-flow-' + flow.side" :key="flow.side" class="gold-flow" :class="'gold-flow-' + flow.side"><span class="flow-aura"></span><span class="flow-glow"></span><span :id="'gold-flow-' + flow.side + '-core'" class="flow-core"></span></div>
    <div class="gold-spark spark-north spark-a"><span></span></div><div class="gold-spark spark-north spark-b"><span></span></div>
    <div class="gold-spark spark-north spark-c"><span></span></div><div class="gold-spark spark-east spark-d"><span></span></div>
    <div class="gold-spark spark-east spark-e"><span></span></div><div class="gold-spark spark-south spark-f"><span></span></div>
    <div class="gold-spark spark-south spark-g"><span></span></div><div class="gold-spark spark-west spark-h"><span></span></div>
    <div class="gold-spark spark-west spark-i"><span></span></div><div class="gold-spark spark-south spark-j"><span></span></div>
  </div>
</template>
