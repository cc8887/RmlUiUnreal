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

async function fixture({ packed = false, cacheMaxEntries, cacheMaxBytes, allocationBytesPerPlan,
  replaceDuringSecondCompiledStart = false } = {}) {
  const frames = new Map(), starts = [], batches = [], controls = [], staleHandles = new Set(), staleNodes = new Set(),
    planRegistrations = [], compiledStarts = [], releases = [];
  let nextFrame = 0, nextHandle = 100;
  let nextPlanHandle = 500;
  const native = {
    OnAnimationEvent: new Delegate(),
    IsNodeValid: node => node > 0,
    GetComputedProperty: (_node, property) => property === 'transform' ? 'none' :
      /(?:left|right|top|bottom|width|height)$/.test(property) ? '0px' : '0',
    StartNodeKeyframeAnimation: (_node, property) => JSON.stringify(
      ['opacity', 'left', 'top', 'right', 'bottom', 'width', 'height'].includes(property)
      ? { accepted: true, route: 'native', handle: '101', state: 'running' }
      : { accepted: false, route: 'rejected', handle: '', state: 'rejected', error: 'unsupported_property' }),
    StartNodeKeyframeAnimationBatch: json => {
      const requests = JSON.parse(json); starts.push(requests);
      const failedIndex = requests.findIndex(request =>
        !['opacity', 'left', 'top', 'right', 'bottom', 'width', 'height'].includes(request.property));
      return failedIndex >= 0
        ? JSON.stringify({ accepted: false, route: 'rejected', handles: [], state: 'rejected', error: 'unsupported_property', failedIndex })
        : JSON.stringify({ accepted: true, route: 'native', handles: requests.map(() => String(++nextHandle)), state: 'running' });
    },
    ControlAnimation: (handle, command, value) => {
      controls.push({ handle, command, value });
      if (command === 'cancel' && staleHandles.has(handle)) {
        return JSON.stringify({ accepted: false, route: 'rejected', handle, state: 'rejected', error: 'stale_handle' });
      }
      if (command === 'cancel') native.OnAnimationEvent.emit(JSON.stringify({ handle, reason: 'cancelled', state: 'cancelled' }));
      return JSON.stringify({ accepted: true, route: 'native', handle,
        state: command === 'pause' ? 'paused' : command === 'cancel' ? 'cancelled' : 'running' });
    },
    ApplyNodePropertyBatch: json => {
      const updates = JSON.parse(json); batches.push(updates);
      const failedIndices = updates.flatMap((update, index) => staleNodes.has(update.node) ? [index] : []);
      if (failedIndices.length)
        return JSON.stringify({ accepted: false, applied: 0, error: 'stale_property_target', failedIndices });
      return JSON.stringify({ accepted: true, applied: updates.length });
    },
    GetAnimationRuntimeStats: () => JSON.stringify({
      activeTracks: 3, visual: 1, layoutPosition: 1, layoutSize: 1,
    }),
  };
  if (packed) {
    native.bUseCompiledAnimationPlans = true;
    if (cacheMaxEntries !== undefined) native.CompiledAnimationPlanCacheMaxEntries = cacheMaxEntries;
    if (cacheMaxBytes !== undefined) native.CompiledAnimationPlanCacheMaxBytes = cacheMaxBytes;
    native.RegisterAnimationPlansPacked = buffer => {
      const view = new DataView(buffer);
      assert.equal(view.getUint32(0, true), 0x31504152);
      const count = view.getUint32(8, true);
      planRegistrations.push({ buffer, count });
      return JSON.stringify({
        accepted: true,
        handles: Array.from({ length: count }, () => String(++nextPlanHandle)),
        ...(allocationBytesPerPlan === undefined ? {} : {
          allocatedBytes: Array.from({ length: count }, () => allocationBytesPerPlan),
        }),
      });
    };
    native.StartCompiledAnimationBatchPacked = buffer => {
      const view = new DataView(buffer);
      assert.equal(view.getUint32(0, true), 0x31494152);
      const count = view.getUint32(8, true);
      compiledStarts.push({ buffer, count });
      if (replaceDuringSecondCompiledStart && compiledStarts.length === 2)
        native.OnAnimationEvent.emit(JSON.stringify({ handle: '101', reason: 'replaced', state: 'replaced' }));
      return JSON.stringify({ accepted: true, route: 'native', handles: Array.from({ length: count }, () => String(++nextHandle)), state: 'running' });
    };
    native.ReleaseAnimationPlansPacked = buffer => {
      const view = new DataView(buffer);
      assert.equal(view.getUint32(0, true), 0x31524152);
      const count = view.getUint32(8, true);
      releases.push({ buffer, count });
      return JSON.stringify({ accepted: true, handles: [], released: count });
    };
  }
  const result = await build({
    entryPoints: [path.resolve('src/animation.ts')], bundle: true, write: false,
    platform: 'browser', format: 'iife', globalName: 'animationModule',
    plugins: [{
      name: 'animation-native-fixture',
      setup(builder) {
        builder.onResolve({ filter: /^\.\/bridge$/ }, () => ({ path: 'bridge', namespace: 'fixture' }));
        builder.onLoad({ filter: /^bridge$/, namespace: 'fixture' }, () => ({
          contents: 'export const native = globalThis.__native; export function report(error) { globalThis.__errors.push(String(error)); }',
          loader: 'js',
        }));
      },
    }],
  });
  const context = vm.createContext({
    __native: native, __errors: [],
    requestAnimationFrame: callback => { const id = ++nextFrame; frames.set(id, callback); return id; },
    cancelAnimationFrame: id => frames.delete(id),
  });
  vm.runInContext(result.outputFiles[0].text, context);
  const runFrame = time => {
    const callbacks = [...frames.values()]; frames.clear();
    for (const callback of callbacks) callback(time);
  };
  return { module: context.animationModule, native, frames, starts, batches, controls, staleHandles, staleNodes,
    planRegistrations, compiledStarts, releases, context, runFrame };
}

test('native completion events settle the animation promise with the opaque handle', async () => {
  const f = await fixture();
  const animation = f.module.startAnimation(1, 'opacity', [
    { offset: 0, value: 0 }, { offset: 1, value: 1 },
  ], { duration: 1 });
  assert.equal(animation.route, 'native');
  assert.equal(animation.handle, '101');
  f.native.OnAnimationEvent.emit(JSON.stringify({ handle: '101', reason: 'completed', state: 'finished' }));
  const completion = await animation.finished;
  assert.equal(completion.handle, '101');
  assert.equal(completion.route, 'native');
  assert.equal(completion.reason, 'completed');
  assert.equal(animation.state, 'finished');
  f.module.disposeAnimations();
  assert.equal(f.native.OnAnimationEvent.callbacks.size, 0);
});

test('native cancellation is idempotent after completion and stale native retirement', async () => {
  const f = await fixture();
  const completed = f.module.startAnimation(1, 'opacity', [
    { offset: 0, value: 0 }, { offset: 1, value: 1 },
  ], { duration: 1 });
  f.native.OnAnimationEvent.emit(JSON.stringify({ handle: completed.handle, reason: 'completed', state: 'finished' }));
  await completed.finished;
  assert.doesNotThrow(() => completed.cancel());

  const retired = f.module.startAnimation(1, 'opacity', [
    { offset: 0, value: 0 }, { offset: 1, value: 1 },
  ], { duration: 1 });
  f.staleHandles.add(retired.handle);
  assert.doesNotThrow(() => retired.cancel());
  assert.equal(retired.state, 'cancelled');
  assert.equal((await retired.finished).reason, 'cancelled');
  assert.deepEqual(f.context.__errors, []);
  f.module.disposeAnimations();
});

test('native batch starts every track in one host call and rejects duplicate targets before crossing', async () => {
  const f = await fixture();
  const animations = f.module.startAnimations([
    { node: 1, property: 'opacity', keyframes: [{ offset: 0, value: 0 }, { offset: 1, value: 1 }], options: { duration: 1 } },
    { node: 2, property: 'opacity', keyframes: [{ offset: 0, value: 1 }, { offset: 1, value: 0 }], options: { duration: 1, delay: 0.2 } },
  ]);
  assert.equal(f.starts.length, 1);
  assert.equal(f.starts[0].length, 2);
  assert.deepEqual(animations.map(animation => animation.handle), ['101', '102']);
  assert.throws(() => f.module.startAnimations([
    { node: 3, property: 'opacity', keyframes: [{ offset: 0, value: 0 }, { offset: 1, value: 1 }], options: { duration: 1 } },
    { node: 3, property: 'OPACITY', keyframes: [{ offset: 0, value: 1 }, { offset: 1, value: 0 }], options: { duration: 1 } },
  ]), /duplicate target\/property/);
  assert.equal(f.starts.length, 1);
  for (const animation of animations) animation.cancel();
  f.module.disposeAnimations();
});

test('compiled packed plans register once, reuse across batches and release after active bindings cancel', async () => {
  const f = await fixture({ packed: true, allocationBytesPerPlan: 512 });
  const request = node => ({
    node, property: 'opacity',
    keyframes: [{ offset: 0, value: 0.25, easing: 'linear' }, { offset: 1, value: 1 }],
    options: { duration: 0.2, fallback: 'reject' },
  });
  const first = f.module.startAnimations([request(1), request(2)]);
  assert.equal(f.starts.length, 0);
  assert.equal(f.planRegistrations.length, 1);
  assert.equal(f.planRegistrations[0].count, 1, 'identical definitions are interned before registration');
  assert.equal(f.compiledStarts[0].count, 2);
  const debug = f.module.getAnimationStartDebugState();
  assert.equal(debug.transport, 'compiled-packed');
  assert.equal(debug.registrations, 1);
  assert.equal(debug.cacheHits, 1);
  assert.equal(debug.batches, 1);
  assert.equal(debug.cachedPlans, 1);
  assert.equal(debug.cacheBytes, 512);
  assert.equal(debug.activeBindings, 2);
  assert.equal(debug.evictions, 0);
  assert.deepEqual({ ...debug.cachedPlansByCost }, { visual: 1, visualDiscrete: 0, paint: 0, layoutPosition: 0, layoutSize: 0 });
  assert.deepEqual({ ...debug.activeTracksByCost }, {
    activeTracks: 3, visual: 1, layoutPosition: 1, layoutSize: 1,
  });
  assert.equal(first[0].costClass, 'visual');
  for (const animation of first) animation.cancel();

  const second = f.module.startAnimations([request(3)])[0];
  assert.equal(f.planRegistrations.length, 1, 'the second batch reuses the context plan');
  assert.equal(f.compiledStarts.length, 2);
  assert.equal(f.module.getAnimationStartDebugState().cacheHits, 2);
  f.module.disposeAnimations();
  assert.equal(second.state, 'cancelled');
  assert.equal(f.releases.length, 1);
  assert.equal(f.releases[0].count, 1);
});

test('compiled packed px layout plans stay on the native MovieScene path', async () => {
  const f = await fixture({ packed: true });
  const animation = f.module.startAnimation(7, 'left', [
    { offset: 0, value: '-32px', easing: 'ease-out' },
    { offset: 1, value: '48px' },
  ], { duration: 0.4, fallback: 'reject' });
  assert.equal(animation.route, 'native');
  assert.equal(animation.costClass, 'layout-position');
  assert.equal(f.module.getAnimationPropertyCostClass('WIDTH'), 'layout-size');
  assert.equal(f.module.getAnimationPropertyCostClass('unknown'), undefined);
  assert.equal(f.planRegistrations.length, 1);
  const planView = new DataView(f.planRegistrations[0].buffer);
  assert.equal(planView.getUint16(4, true), 3, 'steps-capable packed plans use protocol version 3');
  assert.equal(planView.getUint8(12), 3, 'left uses the stable ABI property id');
  assert.equal(planView.getUint8(44), 3, 'the default fill mode is both');
  assert.equal(f.planRegistrations[0].buffer.byteLength, 108, 'layout scalar keyframes use the compact float encoding');
  assert.equal(f.compiledStarts.length, 1);
  assert.equal(f.batches.length, 0, 'native layout animation does not enqueue JS property commits');
  assert.deepEqual({ ...f.module.getAnimationStartDebugState().cachedPlansByCost },
    { visual: 0, visualDiscrete: 0, paint: 0, layoutPosition: 1, layoutSize: 0 });
  animation.cancel();
  f.module.disposeAnimations();
});

test('compiled plan cache evicts least-recently-used inactive plans before registration', async () => {
  const f = await fixture({ packed: true, cacheMaxEntries: 2 });
  const start = (node, duration) => f.module.startAnimation(node, 'opacity', [
    { offset: 0, value: 0 }, { offset: 1, value: 1 },
  ], { duration, fallback: 'reject' });
  start(1, 0.1).cancel();
  start(2, 0.2).cancel();
  start(3, 0.3).cancel();
  const debug = f.module.getAnimationStartDebugState();
  assert.equal(f.planRegistrations.length, 3);
  assert.equal(f.releases.length, 1);
  assert.equal(f.releases[0].count, 1);
  assert.equal(debug.cachedPlans, 2);
  assert.equal(debug.cacheBytes, 560);
  assert.equal(debug.activeBindings, 0);
  assert.equal(debug.evictions, 1);
  assert.equal(debug.budgetPressure, 0);
  f.module.disposeAnimations();
});

test('compiled plan cache keeps active plans during soft-budget pressure and trims after completion', async () => {
  const f = await fixture({ packed: true, cacheMaxEntries: 1 });
  const request = (node, duration) => ({
    node, property: 'opacity', keyframes: [{ offset: 0, value: 0 }, { offset: 1, value: 1 }],
    options: { duration, fallback: 'reject' },
  });
  const active = f.module.startAnimations([request(1, 1)])[0];
  const transient = f.module.startAnimations([request(2, 2)])[0];
  assert.equal(f.module.getAnimationStartDebugState().cachedPlans, 2);
  assert.equal(f.module.getAnimationStartDebugState().budgetPressure, 1);
  transient.cancel();
  const debug = f.module.getAnimationStartDebugState();
  assert.equal(debug.cachedPlans, 1);
  assert.equal(debug.activeBindings, 1);
  assert.equal(debug.evictions, 1);
  assert.equal(f.releases.length, 1);
  active.cancel();
  f.module.disposeAnimations();
});

test('compiled plan reservation protects a new binding during synchronous replacement callbacks', async () => {
  const f = await fixture({ packed: true, cacheMaxEntries: 1, replaceDuringSecondCompiledStart: true });
  const request = (duration) => ({
    node: 1, property: 'opacity', keyframes: [{ offset: 0, value: 0 }, { offset: 1, value: 1 }],
    options: { duration, fallback: 'reject' },
  });
  const first = f.module.startAnimations([request(1)])[0];
  const second = f.module.startAnimations([request(2)])[0];
  assert.equal(first.state, 'replaced');
  const debug = f.module.getAnimationStartDebugState();
  assert.equal(debug.cachedPlans, 1);
  assert.equal(debug.activeBindings, 1);
  assert.equal(debug.evictions, 1);
  assert.equal(f.releases.length, 1);
  second.cancel();
  f.module.disposeAnimations();
});

test('layered native batch keeps same-target controllers and requires unique contribution order', async () => {
  const f = await fixture();
  const request = (order, delay) => ({
    node: 3,
    property: 'opacity',
    keyframes: [{ offset: 0, value: order * 0.25 }, { offset: 1, value: 1 }],
    options: {
      duration: 1,
      delay,
      composite: 'layered-replace',
      compositionOrder: order,
      fallback: 'reject',
    },
  });
  const animations = f.module.startAnimations([request(0, 0), request(1, 0.5)]);
  assert.equal(animations.length, 2);
  assert.equal(f.starts.length, 1);
  assert.deepEqual(f.starts[0].map(entry => entry.options.compositionOrder), [0, 1]);
  f.native.OnAnimationEvent.emit(JSON.stringify({ handle: animations[1].handle, reason: 'completed' }));
  assert.equal((await animations[1].finished).reason, 'completed');
  assert.equal(animations[0].state, 'running');
  assert.throws(() => f.module.startAnimations([request(2, 0), request(2, 0.5)]), /contribution orders/);
  assert.equal(f.starts.length, 1);
  animations[0].cancel();
  f.module.disposeAnimations();
});

test('zero-duration native tracks preserve exact timing and never enter fallback', async () => {
  const f = await fixture();
  const animation = f.module.startAnimation(1, 'opacity', [
    { offset: 0, value: 0 }, { offset: 1, value: 1 },
  ], { duration: 0, delay: 0.25, fallback: 'reject' });
  assert.equal(animation.route, 'native');
  assert.equal(f.starts.length, 1);
  assert.equal(f.starts[0][0].options.duration, 0);
  assert.equal(f.starts[0][0].options.delay, 0.25);
  assert.equal(f.frames.size, 0);
  animation.cancel();
  f.module.disposeAnimations();
});

test('scalar fallback batches all tracks into one bridge call and schedules only active work', async () => {
  const f = await fixture();
  const width = f.module.startAnimation(1, 'margin-left', [
    { offset: 0, value: '0px' }, { offset: 1, value: '100px', easing: 'ease-in-out' },
  ], { duration: 1 });
  const height = f.module.startAnimation(2, 'padding-left', [
    { offset: 0, value: '10px' }, { offset: 1, value: '30px' },
  ], { duration: 1 });
  assert.equal(width.route, 'js_batched');
  assert.equal(f.frames.size, 1);

  f.runFrame(0);
  assert.equal(f.batches.length, 1);
  assert.equal(f.batches[0].length, 2);
  assert.equal(f.frames.size, 1);
  f.runFrame(500);
  assert.equal(f.batches.length, 2);
  assert.equal(f.batches[1].length, 2);
  assert.equal(f.batches[1].find(update => update.property === 'padding-left').value, '20px');

  width.pause(); height.pause();
  f.runFrame(600);
  assert.equal(f.batches.length, 2, 'paused tracks do not submit another batch');
  assert.equal(f.frames.size, 0, 'paused tracks leave the rAF schedule');

  width.seek(0.75);
  assert.equal(f.frames.size, 1);
  f.runFrame(700);
  assert.equal(f.batches.length, 3);
  assert.equal(f.batches[2].length, 1, 'paused seek samples only the dirty track');
  assert.equal(f.frames.size, 0);

  width.cancel(); height.cancel();
  assert.equal((await width.finished).reason, 'cancelled');
  assert.equal((await height.finished).reason, 'cancelled');
  f.module.disposeAnimations();
  assert.deepEqual(f.context.__errors, []);
});

test('fallback fill modes control delay contribution and completion restoration', async () => {
  const cases = [
    { fill: 'none', before: 0, final: '0px' },
    { fill: 'forwards', before: 0, final: '20px' },
    { fill: 'backwards', before: 1, final: '0px' },
    { fill: 'both', before: 1, final: '20px' },
  ];
  for (const item of cases) {
    const f = await fixture();
    const animation = f.module.startAnimation(1, 'margin-left', [
      { offset: 0, value: '10px' }, { offset: 1, value: '20px' },
    ], { duration: 1, delay: 0.5, fill: item.fill });
    f.runFrame(0);
    assert.equal(f.batches.length, item.before, `${item.fill} delay contribution`);
    f.runFrame(500);
    assert.equal(f.batches.at(-1)[0].value, '10px');
    f.runFrame(1500);
    assert.equal(f.batches.at(-1)[0].value, item.final, `${item.fill} completion value`);
    assert.equal((await animation.finished).reason, 'completed');
    f.module.disposeAnimations();
    assert.deepEqual(f.context.__errors, []);
  }
});

test('steps easing preserves discontinuities in fallback and RAP3 plans', async () => {
  const fallback = await fixture();
  fallback.module.startAnimation(1, 'margin-left', [
    { offset: 0, value: '0px', easing: 'steps(4, jump-end)' },
    { offset: 1, value: '100px' },
  ], { duration: 1 });
  fallback.runFrame(0);
  fallback.runFrame(240);
  assert.equal(fallback.batches.at(-1)[0].value, '0px');
  fallback.runFrame(250);
  assert.equal(fallback.batches.at(-1)[0].value, '25px');
  fallback.module.disposeAnimations();

  const packed = await fixture({ packed: true });
  packed.module.startAnimation(1, 'opacity', [
    { offset: 0, value: 0, easing: 'step-start' }, { offset: 1, value: 1 },
  ], { duration: 1, fallback: 'reject' });
  const view = new DataView(packed.planRegistrations[0].buffer);
  assert.equal(view.getUint16(4, true), 3);
  assert.equal(view.getUint8(60), 2, 'steps uses the stable easing type id');
  assert.equal(view.getFloat32(64, true), 1, 'step-start encodes one step');
  assert.equal(view.getFloat32(68, true), 1, 'step-start encodes jump-start');
  packed.module.disposeAnimations();
});

test('packed plan cache separates fill modes and encodes them in RAP v3', async () => {
  const f = await fixture({ packed: true });
  const start = fill => f.module.startAnimation(1, 'opacity', [
    { offset: 0, value: 0 }, { offset: 1, value: 1 },
  ], { duration: 1, fill, fallback: 'reject' });
  const none = start('none');
  none.cancel();
  const both = start('both');
  assert.equal(f.planRegistrations.length, 2);
  assert.deepEqual(f.planRegistrations.map(entry => new DataView(entry.buffer).getUint8(44)), [0, 3]);
  both.cancel();
  f.module.disposeAnimations();
});

test('scalar fallback cancels removed targets and retries live tracks without reporting an error', async () => {
  const f = await fixture();
  const staleWidth = f.module.startAnimation(1, 'margin-left', [
    { offset: 0, value: '0px' }, { offset: 1, value: '100px' },
  ], { duration: 1 });
  const staleHeight = f.module.startAnimation(1, 'padding-left', [
    { offset: 0, value: '10px' }, { offset: 1, value: '30px' },
  ], { duration: 1 });
  const liveWidth = f.module.startAnimation(2, 'margin-left', [
    { offset: 0, value: '20px' }, { offset: 1, value: '60px' },
  ], { duration: 1 });
  f.staleNodes.add(1);

  f.runFrame(0);
  assert.equal(f.batches.length, 2, 'one rejected batch is followed by one live-only retry');
  assert.equal(f.batches[0].length, 3);
  assert.deepEqual(f.batches[1].map(update => update.node), [2]);
  assert.equal((await staleWidth.finished).reason, 'cancelled');
  assert.equal((await staleHeight.finished).reason, 'cancelled');
  assert.equal(liveWidth.state, 'running');
  assert.equal(f.frames.size, 1);
  assert.deepEqual(f.context.__errors, []);

  f.runFrame(1000);
  assert.equal((await liveWidth.finished).reason, 'completed');
  f.module.disposeAnimations();
});
