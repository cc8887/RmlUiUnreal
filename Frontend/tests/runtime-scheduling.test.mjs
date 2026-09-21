import test from 'node:test';
import assert from 'node:assert/strict';
import vm from 'node:vm';
import path from 'node:path';
import { build } from 'esbuild';

class Delegate {
  callbacks = new Set();
  Add(callback) { this.callbacks.add(callback); }
  Remove(callback) { this.callbacks.delete(callback); }
  emit(...args) { for (const callback of [...this.callbacks]) callback(...args); }
}

test('runtime publishes timer and animation-frame deadlines without browser globals', async () => {
  const schedules = [];
  const native = {
    OnHostResponse: new Delegate(),
    OnFrame: new Delegate(),
    OnAnimationEvent: new Delegate(),
    SetWakeSchedule: (delay, frame) => schedules.push([delay, frame]),
  };
  const result = await build({
    entryPoints: [path.resolve('src/runtime.ts')],
    bundle: true,
    write: false,
    platform: 'browser',
    format: 'iife',
    globalName: 'runtimeModule',
    plugins: [{
      name: 'runtime-native-fixture',
      setup(builder) {
        builder.onResolve({ filter: /^\.\/bridge$/ }, () => ({ path: 'bridge', namespace: 'fixture' }));
        builder.onResolve({ filter: /^\.\/platform$/ }, () => ({ path: 'platform', namespace: 'fixture' }));
        builder.onLoad({ filter: /^bridge$/, namespace: 'fixture' }, () => ({
          contents: 'export const native = globalThis.__native; export function report(error) { throw error; }',
          loader: 'js',
        }));
        builder.onLoad({ filter: /^platform$/, namespace: 'fixture' }, () => ({
          contents: 'export function disposePlatform() {}', loader: 'js',
        }));
      },
    }],
  });
  const context = vm.createContext({ __native: native });
  vm.runInContext(result.outputFiles[0].text, context);
  assert.deepEqual(schedules.at(-1), [-1, false]);

  let timeoutCalls = 0;
  context.setTimeout(() => timeoutCalls++, 100);
  assert.deepEqual(schedules.at(-1), [100, false]);
  native.OnFrame.emit(0.04);
  assert.equal(timeoutCalls, 0);
  assert.deepEqual(schedules.at(-1), [60, false]);
  native.OnFrame.emit(0.06);
  assert.equal(timeoutCalls, 1);
  assert.deepEqual(schedules.at(-1), [-1, false]);

  let frameTime = -1;
  context.requestAnimationFrame(time => { frameTime = time; });
  assert.equal(schedules.at(-1)[1], true);
  native.OnFrame.emit(0.016);
  assert.equal(frameTime, 116);
  assert.deepEqual(schedules.at(-1), [-1, false]);

  const interval = context.setInterval(() => {}, 25);
  assert.deepEqual(schedules.at(-1), [25, false]);
  context.clearInterval(interval);
  assert.deepEqual(schedules.at(-1), [-1, false]);
  context.runtimeModule.disposeRuntime();
  assert.deepEqual(schedules.at(-1), [-1, false]);
});
