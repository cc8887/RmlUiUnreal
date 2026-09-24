<script setup lang="ts">
import { onBeforeUnmount, onMounted, ref, watch, type RmlNode } from '@rmlui/vue';
import { check, native } from '../bridge';
import { renderEditableChartSvg, type EditableChartDatum } from './echartsSvg';

const props = defineProps<{ data: readonly EditableChartDatum[] }>();
const host = ref<RmlNode | null>(null);
let renderCount = 0;

function render(): void {
  if (!host.value) return;
  check(native.SetInnerRml(host.value.handle, renderEditableChartSvg(props.data)));
  native.SetAttribute(host.value.handle, 'data-render-count', String(++renderCount), false);
}

onMounted(render);
watch(() => props.data.map(item => [item.label, item.value, item.target]), render);
onBeforeUnmount(() => { if (host.value) native.SetInnerRml(host.value.handle, ''); });
</script>

<template><div id="echarts-svg-root" ref="host" class="echarts-svg-host"></div></template>
