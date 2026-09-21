import { native, report } from './bridge';
import { disposePlatform } from './platform';
import { disposeAnimations } from './animation';

let nextRequest = 0, nextTimer = 0, clock = 0, disposed = false;
const pending = new Map<number, { resolve: (value: any) => void; reject: (error: Error) => void }>();
const timers = new Map<number, { due: number; period: number; callback: () => void }>();
const frames = new Map<number, (time: number) => void>();
/** Explicit JSON compatibility channel. New business APIs should use getService() from bridge.ts. */
export function callHostJson<T>(method: string, payload: unknown): Promise<T> {
  if (disposed) return Promise.reject(new Error('UI context has been disposed'));
  return new Promise<T>((resolve, reject) => {
    const id = ++nextRequest;
    pending.set(id, { resolve, reject });
    native.RequestHost(id, method, JSON.stringify(payload));
  });
}
function receive(json: string): void {
  const response = JSON.parse(json), request = pending.get(response.id);
  if (!request) return;
  pending.delete(response.id);
  if (!response.success) request.reject(new Error(response.payload));
  else { try { request.resolve(JSON.parse(response.payload)); } catch (error) { request.reject(error as Error); } }
}
function schedule(callback: () => void, delay = 0, repeat = false): number {
  const id = ++nextTimer;
  if (!disposed) timers.set(id, { due: clock + Math.max(0, delay), period: repeat ? Math.max(1, delay) : 0, callback });
  publishSchedule();
  return id;
}
function publishSchedule(): void {
  if (disposed) return;
  let delay = -1;
  for (const timer of timers.values()) {
    const candidate = Math.max(0, timer.due - clock);
    if (delay < 0 || candidate < delay) delay = candidate;
  }
  native.SetWakeSchedule(delay, frames.size > 0);
}
Object.assign(globalThis, {
  setTimeout: (callback: () => void, delay = 0) => schedule(callback, delay),
  setInterval: (callback: () => void, delay = 0) => schedule(callback, delay, true),
  clearTimeout: (id: number) => { timers.delete(id); publishSchedule(); },
  clearInterval: (id: number) => { timers.delete(id); publishSchedule(); },
  requestAnimationFrame: (callback: (time: number) => void) => {
    const id = ++nextTimer; frames.set(id, callback); publishSchedule(); return id;
  },
  cancelAnimationFrame: (id: number) => { frames.delete(id); publishSchedule(); },
});
function advance(delta: number): void {
  clock += Math.max(0, delta) * 1000;
  const ready = [...timers].filter(([, timer]) => timer.due <= clock).slice(0, 100);
  for (const [id, timer] of ready) {
    if (!timers.has(id)) continue;
    if (timer.period) timer.due = clock + timer.period; else timers.delete(id);
    try { timer.callback(); } catch (error) { report(error); }
  }
  const callbacks = [...frames.values()]; frames.clear();
  for (const callback of callbacks) { try { callback(clock); } catch (error) { report(error); } }
  publishSchedule();
}
export function disposeRuntime(): void {
  disposeAnimations();
  disposePlatform();
  disposed = true; timers.clear(); frames.clear();
  native.SetWakeSchedule(-1, false);
  for (const request of pending.values()) request.reject(new Error('UI version replaced'));
  pending.clear(); native.OnHostResponse.Remove(receive); native.OnFrame.Remove(advance);
}
native.OnHostResponse.Add(receive); native.OnFrame.Add(advance);
publishSchedule();
