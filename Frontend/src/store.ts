import { reactive, watchEffect } from '@rmlui/vue';
import { findService, native } from './bridge';

export interface Project { id: number; name: string; progress: number }
interface DemoHostService { SaveSession(name: string, projectCount: number, enabled: boolean): string }
const host = findService<DemoHostService>('host');
const initial = { name: 'Northstar session', enabled: true, accent: 'teal', intensity: 72,
  projects: [{ id: 1, name: 'Interface assembly', progress: 82 }, { id: 2, name: 'Style validation', progress: 64 }, { id: 3, name: 'Asset synchronization', progress: 46 }], nextId: 4, saves: 0 };
let restored: Partial<typeof initial> = {};
try { const value = JSON.parse(native.StateJson); if (value.schema === 1) restored = value.data; } catch { /* First mount uses defaults. */ }
export const state = reactive({ ...initial, ...restored, busy: false, status: 'Ready', events: 0, ticks: 0 });
export const version = native.Version;
export function addProject(): void { const id = state.nextId++; state.projects.push({ id, name: `Project ${id}`, progress: 20 }); state.events++; }
export function reverseProjects(): void { state.projects.reverse(); state.events++; }
export function removeProject(id: number): void { state.projects = state.projects.filter(project => project.id !== id); state.events++; }
export async function save(): Promise<void> {
  state.busy = true; state.status = 'Saving';
  try {
    if (!host) throw new Error('Unreal service is not registered: host');
    state.status = host.SaveSession(state.name, state.projects.length, state.enabled);
    state.saves++;
  } catch (error) { state.status = String(error); }
  finally { state.busy = false; }
}
export function serialize(): string {
  return JSON.stringify({ schema: 1, data: { name: state.name, enabled: state.enabled, accent: state.accent,
    intensity: state.intensity, projects: state.projects, nextId: state.nextId, saves: state.saves } });
}
watchEffect(() => native.ReportDebugState(JSON.stringify({ ...state, version })));
