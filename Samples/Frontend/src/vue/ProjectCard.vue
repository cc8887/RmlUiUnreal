<script setup lang="ts">
import type { Project } from './store';
defineProps<{ project: Project }>();
const emit = defineEmits<{ remove: [id: number] }>();
</script>
<template>
  <div class="project">
    <div class="project-heading"><h3>{{ project.name }}</h3><button :id="'vue-remove-' + project.id" class="remove" @click.stop="emit('remove', project.id)">Remove</button></div>
    <p class="muted">In progress</p>
    <div class="track"><div class="fill" :style="{ width: project.progress + '%' }" /></div>
    <p class="percentage">{{ project.progress }}%</p>
  </div>
</template>
<style scoped>
.project-heading { min-height:36px; }
</style>
