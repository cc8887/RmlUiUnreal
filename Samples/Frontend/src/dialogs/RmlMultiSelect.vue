<script setup lang="ts">
import { computed, ref, type RmlEvent } from '@rmlui/vue';
import { native } from '../../../../Frontend/src/bridge';
const props = defineProps<{ id: string; modelValue: string[]; options: { value: string; label: string }[] }>();
const emit = defineEmits<{ 'update:modelValue': [value: string[]] }>();
const active = ref(0);
const value = computed({ get: () => props.modelValue, set: next => emit('update:modelValue', next) });
function keydown(event: RmlEvent, index: number): void {
  const count = props.options.length;
  if (!count) return;
  if (!['ArrowUp', 'ArrowDown', 'Home', 'End'].includes(event.keyName)) return;
  event.preventDefault(); event.stopPropagation();
  active.value = event.keyName === 'Home' ? 0 : event.keyName === 'End' ? count - 1 : (index + (event.keyName === 'ArrowDown' ? 1 : -1) + count) % count;
  const node = native.FindNode(`${props.id}-${active.value}`); if (node) native.FocusNode(node);
}
</script>

<template><div :id="id" class="rml-multiselect" role="group"><label v-for="(option, index) in options" :key="option.value" class="multi-option"><input :id="id + '-' + index" v-model="value" type="checkbox" :value="option.value" :tabindex="index === active ? 0 : -1" @keydown="keydown($event, index)" /><span :id="id + '-' + index + '-label'">{{ option.label }}</span></label></div></template>

<style>
.rml-multiselect { display:flex; gap:12px; padding:10px; border:1px #c5d2d7; background-color:#f7fafb; }
.multi-option { display:flex; gap:7px; align-items:center; font-size:11px; color:#40525b; }
.multi-option input { width:18px; height:18px; flex-shrink:0; box-sizing:border-box; border:2px #78959f; border-radius:3px; background-color:#ffffff; }
.multi-option input:checked { border:5px #0b846f; }
.multi-option input:focus { border-color:#df9b22; background-color:#fff1d0; }
</style>
