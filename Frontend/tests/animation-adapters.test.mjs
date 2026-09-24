import test from 'node:test';
import assert from 'node:assert/strict';
import vm from 'node:vm';
import path from 'node:path';
import fs from 'node:fs';
import { createRequire } from 'node:module';
import { build } from 'esbuild';
import {
  animationAdapterFixturePath,
  animationPlanCacheWorkloadFixturePath,
  buildAnimationAdapterFixture,
  buildAnimationPlanCacheWorkloadFixture,
} from '../tools/build-animation-adapter-fixture.mjs';

const require = createRequire(import.meta.url);

test('committed Puerts adapter fixture matches the TypeScript sources', async () => {
  const [actual, expected] = await Promise.all([
    fs.promises.readFile(animationAdapterFixturePath, 'utf8'),
    buildAnimationAdapterFixture({ write: false }),
  ]);
  assert.equal(actual, expected);
});

test('committed plan-cache workload fixture matches the TypeScript sources', async () => {
  const [actual, expected] = await Promise.all([
    fs.promises.readFile(animationPlanCacheWorkloadFixturePath, 'utf8'),
    buildAnimationPlanCacheWorkloadFixture({ write: false }),
  ]);
  assert.equal(actual, expected);
});

function packageVersion(specifier, expectedName) {
  let directory = path.dirname(require.resolve(specifier));
  while (directory !== path.dirname(directory)) {
    const manifest = path.join(directory, 'package.json');
    if (fs.existsSync(manifest)) {
      const value = JSON.parse(fs.readFileSync(manifest, 'utf8'));
      if (value.name === expectedName) return value.version;
    }
    directory = path.dirname(directory);
  }
  throw new Error(`Unable to locate ${expectedName}`);
}

function cubicBezierValue(points, x) {
  const coordinate = (t, first, second) => {
    const inverse = 1 - t;
    return 3 * inverse * inverse * t * first + 3 * inverse * t * t * second + t * t * t;
  };
  let low = 0, high = 1;
  for (let index = 0; index < 48; ++index) {
    const middle = (low + high) * 0.5;
    if (coordinate(middle, points[0], points[2]) < x) low = middle; else high = middle;
  }
  return coordinate((low + high) * 0.5, points[1], points[3]);
}

function parseCubicBezier(text) {
  const match = text.match(/^cubic-bezier\(([^,]+),([^,]+),([^,]+),([^\)]+)\)$/);
  assert.ok(match, `Expected cubic-bezier easing, received ${text}`);
  return match.slice(1).map(Number);
}

class Delegate {
  callbacks = new Set();
  Add(callback) { this.callbacks.add(callback); }
  Remove(callback) { this.callbacks.delete(callback); }
  emit(...args) { for (const callback of [...this.callbacks]) callback(...args); }
}

async function fixture() {
  const frames = new Map(), starts = [], batchStarts = [], batches = [], controls = [], snapshotCalls = [];
  let nextFrame = 0, nextHandle = 100;
  const computed = new Map([
    ['1:opacity', '0'], ['2:opacity', '0.25'],
    ['1:visibility', 'visible'], ['2:visibility', 'visible'],
    ['1:background-color', '#000000ff'], ['2:background-color', '#000000ff'],
    ['1:width', '10px'], ['2:width', '20px'],
    ['1:margin-left', '0px'], ['2:margin-left', '0px'],
    ['1:transform', 'none'], ['2:transform', 'none'],
    ['3:transform', 'none'], ['4:transform', 'none'], ['5:transform', 'none'],
    ['4:opacity', '1'], ['5:opacity', '1'],
  ]);
  const native = {
    OnAnimationEvent: new Delegate(),
    RootNode: () => 99,
    QueryNodes: (_root, selector) => JSON.stringify(selector === '.items' ? [1, 2] : selector === '.one' ? [1] : []),
    IsNodeValid: node => node >= 1 && node <= 5,
    GetComputedProperty: (node, property) => computed.get(`${node}:${property}`) || '',
    ResolveAnimationHostSnapshot: json => {
      const request = JSON.parse(json); snapshotCalls.push(request);
      const nodes = [];
      const targetGroups = [];
      for (const target of request.targets) {
        const handles = typeof target === 'number' ? [target]
          : target === '.items' ? [1, 2]
            : target === '.one' ? [1]
              : target === '#official-field' ? [3]
                : target === '#official-ball' ? [4]
                  : target === '#official-effect' ? [5]
                    : [];
        targetGroups.push(handles);
        for (const node of handles) {
          if (nodes.some(entry => entry.node === node)) continue;
          nodes.push({
            node,
            properties: Object.fromEntries(request.properties.map(property => [property, computed.get(`${node}:${property}`) || ''])),
            ...(request.includeMetrics ? { metrics: {
              visible: true, x: 0, y: 0,
              width: node === 3 ? 500 : node === 4 ? 64 : 100,
              height: node === 3 ? 500 : node === 4 ? 64 : 20,
              clientWidth: node === 3 ? 500 : node === 4 ? 64 : 100,
              clientHeight: node === 3 ? 500 : node === 4 ? 64 : 20,
            } } : {}),
          });
        }
      }
      return JSON.stringify(nodes.length
        ? { accepted: true, revision: '7', viewport: { width: 320, height: 200, dpi: 1 }, nodes, targetGroups }
        : { accepted: false, error: 'snapshot_target_not_found' });
    },
    StartNodeKeyframeAnimation: (node, property, keyframesJson, optionsJson) => {
      const keyframes = JSON.parse(keyframesJson), options = JSON.parse(optionsJson);
      starts.push({ node, property, keyframes, options });
      const unsupportedEasing = keyframes.some(frame => String(frame.easing || '').startsWith('rml-power('));
      if (unsupportedEasing) return JSON.stringify({ accepted: false, route: 'rejected', handle: '', state: 'rejected', error: 'unsupported_easing' });
      if (!['opacity', 'transform', 'left', 'top', 'right', 'bottom', 'width', 'height', 'visibility', 'color', 'background-color', 'border-color', 'image-color'].includes(property)) return JSON.stringify({ accepted: false, route: 'rejected', handle: '', state: 'rejected', error: 'unsupported_property' });
      return JSON.stringify({ accepted: true, route: 'native', handle: String(++nextHandle), state: 'running' });
    },
    StartNodeKeyframeAnimationBatch: json => {
      const requests = JSON.parse(json);
      batchStarts.push(requests);
      starts.push(...requests.map(({ node, property, keyframes, options }) => ({ node, property, keyframes, options })));
      const failedIndex = requests.findIndex(request =>
        !['opacity', 'transform', 'left', 'top', 'right', 'bottom', 'width', 'height', 'visibility', 'color', 'background-color', 'border-color', 'image-color'].includes(request.property) ||
        request.keyframes.some(frame => String(frame.easing || '').startsWith('rml-power(')));
      if (failedIndex >= 0) {
        const request = requests[failedIndex];
        const error = !['opacity', 'transform', 'left', 'top', 'right', 'bottom', 'width', 'height', 'visibility', 'color', 'background-color', 'border-color', 'image-color'].includes(request.property) ? 'unsupported_property' : 'unsupported_easing';
        return JSON.stringify({ accepted: false, route: 'rejected', handles: [], state: 'rejected', error, failedIndex });
      }
      return JSON.stringify({
        accepted: true, route: 'native', state: 'running',
        handles: requests.map(() => String(++nextHandle)),
      });
    },
    ControlAnimation: (handle, command, value) => {
      controls.push({ handle, command, value });
      if (command === 'cancel') native.OnAnimationEvent.emit(JSON.stringify({ handle, reason: 'cancelled', state: 'cancelled' }));
      return JSON.stringify({ accepted: true, route: 'native', handle, state: command === 'pause' ? 'paused' : 'running' });
    },
    ApplyNodePropertyBatch: json => {
      const updates = JSON.parse(json); batches.push(updates);
      return JSON.stringify({ accepted: true, applied: updates.length });
    },
  };
  const result = await build({
    entryPoints: [path.resolve('src/animation-adapters.ts')], bundle: true, write: false,
    platform: 'browser', format: 'iife', globalName: 'adapterModule',
    plugins: [{
      name: 'animation-adapter-native-fixture',
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
  return { module: context.adapterModule, native, frames, starts, batchStarts, batches, controls, snapshotCalls, context, runFrame };
}

test('HostSnapshot preserves target-spec groups while deduplicating node payloads', async () => {
  const f = await fixture();
  const snapshot = f.module.captureAnimationHostSnapshot([1, '.items'], ['opacity']);
  assert.equal(JSON.stringify(snapshot.targetGroups), JSON.stringify([[1], [1, 2]]));
  assert.equal(JSON.stringify(snapshot.nodes.map(node => node.node)), JSON.stringify([1, 2]));
  assert.equal(f.snapshotCalls.length, 1);
});

test('locked upstream packages expose and execute their original object-animation APIs', async () => {
  assert.equal(packageVersion('@olton/animation', '@olton/animation'), '0.5.0');
  assert.equal(packageVersion('animejs', 'animejs'), '4.5.0');
  assert.equal(packageVersion('gsap', 'gsap'), '3.15.0');

  const oldElement = globalThis.HTMLElement;
  const oldRequest = globalThis.requestAnimationFrame;
  const oldCancel = globalThis.cancelAnimationFrame;
  const queued = [];
  globalThis.HTMLElement = class HTMLElement {};
  globalThis.requestAnimationFrame = callback => { queued.push(callback); return queued.length; };
  globalThis.cancelAnimationFrame = () => {};
  try {
    const { animate: animationJs } = await import('@olton/animation');
    const object = new globalThis.HTMLElement();
    object.style = { transform: '' };
    object.value = 0;
    const finished = animationJs({ el: object, draw: { value: [0, 10] }, dur: 1000, ease: 'linear' });
    queued.shift()(performance.now() + 500);
    assert.ok(object.value >= 4 && object.value <= 5);
    queued.shift()(performance.now() + 1100);
    await finished;
    assert.equal(object.value, 10);
  } finally {
    if (oldElement === undefined) delete globalThis.HTMLElement; else globalThis.HTMLElement = oldElement;
    if (oldRequest === undefined) delete globalThis.requestAnimationFrame; else globalThis.requestAnimationFrame = oldRequest;
    if (oldCancel === undefined) delete globalThis.cancelAnimationFrame; else globalThis.cancelAnimationFrame = oldCancel;
  }

  const { animate: anime, createTimeline: createAnimeTimeline, stagger: animeStagger } = await import('animejs');
  const animeTarget = { value: 0 };
  const animeAnimation = anime(animeTarget, { value: 10, duration: 1000, ease: 'linear', autoplay: false });
  animeAnimation.seek(500, true);
  assert.equal(animeTarget.value, 5);
  animeAnimation.cancel();
  const animeTimelineTarget = { value: 0 };
  const animeTimeline = createAnimeTimeline({ autoplay: false, defaults: { duration: 100, ease: 'linear' } })
    .add(animeTimelineTarget, { value: 10 }, 0)
    .add(animeTimelineTarget, { value: 20 }, '<');
  animeTimeline.seek(150, true);
  assert.equal(animeTimelineTarget.value, 15);
  animeTimeline.cancel();
  const animeStaggerTargets = [{ value: 0 }, { value: 0 }];
  const animeStaggerTimeline = createAnimeTimeline({ autoplay: false })
    .add(animeStaggerTargets, { value: 10, duration: 100, ease: 'linear' }, animeStagger(50));
  assert.equal(animeStaggerTimeline.duration, 150);
  animeStaggerTimeline.cancel();

  const animeSetTarget = { value: 0 };
  const animeSetTimeline = createAnimeTimeline({ autoplay: false })
    .add(animeSetTarget, { value: 5, duration: 100, ease: 'linear' }, 0)
    .set(animeSetTarget, { value: 10 }, 100);
  assert.equal(animeSetTimeline.duration, 100);
  animeSetTimeline.seek(99, true);
  assert.equal(animeSetTarget.value, 4.95);
  animeSetTimeline.seek(100, true);
  assert.equal(animeSetTarget.value, 10);
  animeSetTimeline.cancel();

  const animeOverlapTarget = { value: 0 };
  const animeOverlapTimeline = createAnimeTimeline({ autoplay: false })
    .add(animeOverlapTarget, { value: 100, duration: 1000, ease: 'linear' }, 0)
    .add(animeOverlapTarget, { value: 200, duration: 250, ease: 'linear' }, 500);
  animeOverlapTimeline.seek(500, true);
  assert.equal(animeOverlapTarget.value, 50);
  animeOverlapTimeline.seek(625, true);
  assert.equal(animeOverlapTarget.value, 125);
  animeOverlapTimeline.seek(900, true);
  assert.equal(animeOverlapTarget.value, 200);
  animeOverlapTimeline.cancel();

  const animeEasedOverlapTarget = { value: 0 };
  const animeEasedOverlapTimeline = createAnimeTimeline({ autoplay: false })
    .add(animeEasedOverlapTarget, { value: 100, duration: 1000, ease: 'in(2)' }, 0)
    .add(animeEasedOverlapTarget, { value: 200, duration: 250, ease: 'linear' }, 500);
  animeEasedOverlapTimeline.seek(500, true);
  const easedBoundary = 25;
  assert.equal(animeEasedOverlapTarget.value, easedBoundary);
  animeEasedOverlapTimeline.seek(625, true);
  assert.ok(Math.abs(animeEasedOverlapTarget.value - (easedBoundary + 200) * 0.5) < 1e-10);
  animeEasedOverlapTimeline.seek(900, true);
  assert.equal(animeEasedOverlapTarget.value, 200);
  animeEasedOverlapTimeline.cancel();

  const animeKeyframeTarget = { value: 0 };
  const animeKeyframes = anime(animeKeyframeTarget, {
    duration: 1000, ease: 'linear', autoplay: false,
    keyframes: {
      '0%': { value: 0 },
      '50%': { value: 10, ease: 'linear' },
      '100%': { value: 20, ease: 'linear' },
    },
  });
  animeKeyframes.seek(750, true);
  assert.equal(animeKeyframeTarget.value, 15);
  animeKeyframes.cancel();

  const animeDurationTarget = { x: 0, y: 0 };
  const animeDurationKeyframes = anime(animeDurationTarget, {
    autoplay: false, ease: 'linear',
    keyframes: [
      { x: 10, duration: 100, ease: 'linear' },
      { y: 20, duration: 300, ease: 'linear' },
    ],
  });
  assert.equal(animeDurationKeyframes.duration, 400);
  animeDurationKeyframes.seek(250, true);
  assert.equal(animeDurationTarget.x, 10);
  assert.equal(animeDurationTarget.y, 10);
  animeDurationKeyframes.cancel();

  const { gsap } = await import('gsap');
  const gsapTarget = { value: 0 };
  const tween = gsap.to(gsapTarget, { value: 10, duration: 1, ease: 'none', paused: true });
  tween.progress(0.5, true);
  assert.equal(gsapTarget.value, 5);
  tween.kill();

  const gsapRelativeTarget = { value: 10 };
  const relativeTween = gsap.to(gsapRelativeTarget, { value: '+=5', duration: 1, ease: 'none', paused: true });
  relativeTween.progress(1, true);
  assert.equal(gsapRelativeTarget.value, 15);
  relativeTween.kill();

  const staggerTargets = [{ value: 0 }, { value: 0 }];
  const staggerTween = gsap.to(staggerTargets, { value: 10, duration: 1, stagger: 0.2, ease: 'none', paused: true });
  assert.equal(staggerTween.duration(), 1.2);
  staggerTween.seek(0.1, true);
  assert.equal(staggerTargets[0].value, 1);
  assert.equal(staggerTargets[1].value, 0);
  staggerTween.kill();

  const gsapSequenceTarget = { value: 0 };
  const gsapSequence = gsap.to(gsapSequenceTarget, {
    paused: true, duration: 0.9, ease: 'none',
    keyframes: [
      { value: 10, duration: 0.1, ease: 'none' },
      { value: 20, delay: 0.05, duration: 0.3, ease: 'none' },
    ],
  });
  assert.equal(gsapSequence.duration(), 0.9);
  gsapSequence.seek(0.25, true);
  assert.equal(gsapSequenceTarget.value, 10);
  gsapSequence.seek(0.6, true);
  assert.equal(gsapSequenceTarget.value, 15);
  gsapSequence.kill();

  const gsapSetTarget = { value: 0 };
  const gsapSetTimeline = gsap.timeline({ paused: true })
    .to(gsapSetTarget, { value: 5, duration: 0.1, ease: 'none' }, 0)
    .set(gsapSetTarget, { value: 10 }, 0.1);
  assert.equal(gsapSetTimeline.duration(), 0.1);
  gsapSetTimeline.seek(0.099, true);
  assert.equal(gsapSetTarget.value, 4.95);
  gsapSetTimeline.seek(0.1, true);
  assert.equal(gsapSetTarget.value, 10);
  gsapSetTimeline.kill();
});

test('locked GSAP overlap is an activation-time contribution stack', async () => {
  const { gsap } = await import('gsap');
  const create = () => {
    const target = { value: 0 };
    const timeline = gsap.timeline({ paused: true })
      .to(target, { value: 100, duration: 1, ease: 'none' }, 0)
      .to(target, { value: 200, duration: 0.25, ease: 'none' }, 0.5);
    return { target, timeline };
  };

  const sequential = create();
  const sequentialValues = [0.5, 0.625, 0.75, 0.751, 0.9].map(time => {
    sequential.timeline.seek(time, true);
    return sequential.target.value;
  });
  assert.deepEqual(sequentialValues, [50, 125, 200, 75.1, 90]);
  sequential.timeline.kill();

  const crossed = create();
  crossed.timeline.seek(0.625, true);
  assert.equal(crossed.target.value, 131.25, 'first activation captures the underlying value at the seek time');
  crossed.timeline.seek(0.9, true);
  assert.equal(crossed.target.value, 200, 'the first tick crossing completion publishes the upper endpoint');
  crossed.timeline.seek(0.901, true);
  assert.equal(crossed.target.value, 90.1, 'the lower contribution resumes after the upper entity retires');
  crossed.timeline.kill();
});

test('Animation.js mapping preserves milliseconds, total loops, alternate direction and one fallback batch', async () => {
  const f = await fixture();
  let done = 0;
  const group = f.module.adaptAnimationJs({
    el: '.items', draw: { opacity: [0, 1], marginLeft: [10, 30] },
    dur: 500, ease: 'easeInQuad', loop: 2, dir: 'alternate', onDone: () => { ++done; },
  });
  assert.equal(group.animations.length, 4);
  assert.equal(f.snapshotCalls.length, 1);
  assert.equal(f.snapshotCalls[0].targets.length, 1);
  assert.equal(f.snapshotCalls[0].properties.length, 0, 'explicit from/to values do not request computed properties');
  assert.equal(f.starts.length, 4);
  assert.equal(f.starts[0].options.duration, 0.5);
  assert.equal(f.starts[0].options.iterations, 4);
  assert.equal(f.starts[0].options.direction, 'alternate');
  assert.equal(f.starts[0].keyframes[0].easing, 'cubic-bezier(0.333333333333,0,0.666666666667,0.333333333333)');
  assert.equal(f.frames.size, 1);
  f.runFrame(0);
  assert.equal(f.batches.length, 1);
  assert.equal(f.batches[0].length, 4);
  f.runFrame(2000);
  await group.finished;
  assert.equal(done, 1);
  assert.equal(f.context.__errors.length, 0);
});

test('Animation.js official README and Effects cases expose the current compatibility boundary', async () => {
  const f = await fixture();
  const snapshot = f.module.captureAnimationHostSnapshot(
    ['#official-field', '#official-ball'], [], true,
  );
  const field = snapshot.nodes.find(node => node.node === snapshot.targetGroups[0][0]).metrics;
  const ball = snapshot.nodes.find(node => node.node === snapshot.targetGroups[1][0]).metrics;
  const travelX = field.clientWidth - ball.clientWidth;
  const travelY = field.clientHeight - ball.clientHeight;
  assert.equal(travelX, 436, 'README clientWidth calculation is represented by one HostSnapshot');
  assert.equal(travelY, 436, 'README clientHeight calculation is represented by one HostSnapshot');
  assert.deepEqual(f.snapshotCalls[0].targets, ['#official-field', '#official-ball']);
  assert.equal(f.snapshotCalls[0].includeMetrics, true);

  assert.throws(
    () => f.module.adaptAnimationJs({
      el: '#official-ball', draw: { left: [0, travelX] }, dur: 2000,
      ease: 'easeOutQuad', loop: true,
    }),
    error => error.code === 'unsupported_infinite_loop' && error.library === 'animationjs@0.5.0',
    'the exact README infinite loop is rejected instead of silently bounded',
  );
  assert.throws(
    () => f.module.adaptAnimationJs({
      el: '#official-ball', draw: { top: [0, travelY] }, dur: 2000,
      ease: 'easeOutBounce', loop: 1,
    }),
    error => error.code === 'unsupported_easing' && error.library === 'animationjs@0.5.0',
    'the exact README bounce easing is rejected instead of approximated',
  );

  const horizontal = f.module.adaptAnimationJs({
    el: '#official-ball', draw: { left: [0, travelX] }, dur: 2000,
    ease: 'easeOutQuad', loop: 2, dir: 'alternate',
  });
  const rotation = f.module.adaptAnimationJs({
    el: '#official-ball', draw: { rotate: [0, 360] }, dur: 1200, loop: 2,
  });
  const fade = f.module.adaptAnimationJs({
    el: '#official-effect', draw: { opacity: [0, 1] }, dur: 300, ease: 'linear',
  });
  const slide = f.module.adaptAnimationJs({
    el: '#official-effect', draw: { left: [-100, 0], opacity: [0, 1] }, dur: 300, ease: 'linear',
  });
  const zoom = f.module.adaptAnimationJs({
    el: '#official-effect', draw: { scale: [3, 1], opacity: [0, 1] }, dur: 300, ease: 'linear',
  });

  assert.equal(horizontal.animations[0].route, 'native', 'README horizontal movement uses the native px scalar track');
  assert.equal(rotation.animations[0].route, 'native', 'README rotation uses the native transform track');
  assert.equal(fade.animations[0].route, 'native', 'official fade uses native visual opacity');
  assert.ok(slide.animations.every(animation => animation.route === 'native'), 'official slide uses native opacity and px scalar tracks');
  assert.ok(zoom.animations.every(animation => animation.route === 'native'), 'official zoom uses native opacity and transform tracks');
  assert.equal(f.starts.find(start => start.property === 'left')?.options.duration, 2);
  assert.equal(f.starts.find(start => start.property === 'transform')?.keyframes.at(-1).value, 'rotate(360deg)');

  const swirl = f.module.adaptAnimationJs({
    el: '#official-effect',
    draw: { scale: [3, 1], rotate: [180, 0], opacity: [0, 1] },
    dur: 300, ease: 'linear',
  });
  assert.equal(swirl.animations.length, 2, 'official swirl merges scale and rotate into one transform plus opacity');
  assert.ok(swirl.animations.every(animation => animation.route === 'native'));
  const swirlTransform = f.starts.find(start =>
    start.property === 'transform' && String(start.keyframes[0].value).includes('scale(3,3)'));
  assert.equal(swirlTransform?.keyframes[0].value, 'translate(0px,0px) scale(3,3) rotate(180deg)');
  assert.equal(swirlTransform?.keyframes[1].value, 'translate(0px,0px) scale(1,1) rotate(0deg)');

  for (const group of [horizontal, rotation, fade, slide, zoom, swirl]) group.cancel();
});

test('Animation.js stagger expands targets into one native batch with second-based delays', async () => {
  const f = await fixture();
  f.module.adaptAnimationJs({
    el: '.items', draw: { opacity: [0, 1], translateY: [34, -2] },
    dur: 780, ease: 'cubic-bezier(0.16,1.32,0.3,1)', stagger: { each: 0.082 },
  });
  assert.equal(f.snapshotCalls.length, 1);
  assert.equal(f.batchStarts.length, 1);
  assert.equal(f.starts.length, 4);
  assert.deepEqual(f.starts.filter(start => start.property === 'opacity').map(start => start.options.delay), [0, 0.082]);
  assert.deepEqual(f.starts.filter(start => start.property === 'transform').map(start => start.options.delay), [0, 0.082]);
  assert.ok(f.starts.every(start => start.keyframes[0].easing === 'cubic-bezier(0.16,1.32,0.3,1)'));
});

test('AnimationGroup cancellation is safe after native completion', async () => {
  const f = await fixture();
  const group = f.module.adaptAnimationJs({
    el: '.items', draw: { opacity: [0, 1] }, dur: 780,
  });
  for (const animation of group.animations) {
    f.native.OnAnimationEvent.emit(JSON.stringify({
      handle: animation.handle, reason: 'completed', state: 'finished',
    }));
  }
  await group.finished;
  assert.doesNotThrow(() => group.cancel());
  assert.deepEqual(f.context.__errors, []);
});

test('Anime.js mapping uses repeat plus one and settles its completion from native events', async () => {
  const f = await fixture();
  let completed = 0;
  const group = f.module.adaptAnimeJs('.one', {
    opacity: 1, duration: 750, loop: 1, alternate: true, ease: 'linear',
    onComplete: () => { ++completed; },
  });
  assert.equal(group.animations.length, 1);
  assert.equal(f.snapshotCalls.length, 1);
  assert.equal(f.snapshotCalls[0].properties[0], 'opacity');
  assert.equal(f.starts[0].keyframes[0].value, '0');
  assert.equal(f.starts[0].options.duration, 0.75);
  assert.equal(f.starts[0].options.iterations, 2);
  assert.equal(f.starts[0].options.direction, 'alternate');
  const handle = group.animations[0].handle;
  f.native.OnAnimationEvent.emit(JSON.stringify({ handle, reason: 'completed', state: 'finished' }));
  await group.finished;
  assert.equal(completed, 1);
});

test('Anime.js and GSAP preserve CSS step easing for native execution', async () => {
  const f = await fixture();
  f.module.adaptAnimeJs(1, {
    opacity: 1, duration: 100, ease: 'step-start',
  });
  f.module.adaptGsapTo(1, {
    opacity: 1, duration: 0.1, ease: 'steps(4, jump-end)',
  });

  assert.equal(f.starts.length, 2);
  assert.equal(f.starts[0].keyframes[0].easing, 'step-start');
  assert.equal(f.starts[1].keyframes[0].easing, 'steps(4, jump-end)');
});

test('SourceRequest resolves relative values independently from one HostSnapshot', async () => {
  const f = await fixture();
  f.module.adaptGsapTo('.items', {
    opacity: '+=0.5', duration: 1, ease: 'none',
  });
  assert.equal(f.snapshotCalls.length, 1);
  assert.deepEqual([...f.snapshotCalls[0].properties], ['opacity']);
  assert.equal(f.starts.length, 2);
  assert.deepEqual(
    f.starts.map(start => start.keyframes.map(frame => frame.value)),
    [['0', 0.5], ['0.25', 0.75]],
  );
});

test('Anime percentage keyframes compile to native offsets and segment easing', async () => {
  const f = await fixture();
  f.module.adaptAnimeJs('.one', {
    duration: 1000,
    ease: 'linear',
    keyframes: {
      '0%': { opacity: 0 },
      '50%': { opacity: 0.4, ease: 'linear' },
      '100%': { opacity: 1, ease: 'ease-in' },
    },
  });
  assert.equal(f.snapshotCalls.length, 1);
  assert.equal(f.snapshotCalls[0].properties.length, 0);
  assert.equal(f.starts.length, 1);
  assert.deepEqual(f.starts[0].keyframes.map(frame => frame.offset), [0, 0.5, 1]);
  assert.deepEqual(f.starts[0].keyframes.map(frame => frame.value), [0, 0.4, 1]);
  assert.equal(f.starts[0].keyframes[0].easing, 'linear');
  assert.equal(f.starts[0].keyframes[1].easing, 'ease-in');
  assert.equal(f.frames.size, 0, 'native keyframes do not allocate a JS animation frame');
});

test('Anime duration keyframes preserve per-segment timing and missing-property holds', async () => {
  const f = await fixture();
  f.module.adaptAnimeJs(1, {
    ease: 'linear', delay: 25,
    keyframes: [
      { opacity: 0.4, duration: 100, ease: 'linear' },
      { duration: 300 },
      { opacity: 1, delay: 100, duration: 100, ease: 'ease-in' },
    ],
  });
  assert.equal(f.snapshotCalls.length, 1);
  assert.deepEqual([...f.snapshotCalls[0].properties], ['opacity']);
  assert.equal(f.starts.length, 1);
  assert.equal(f.starts[0].options.delay, 0.025);
  assert.equal(f.starts[0].options.duration, 0.6);
  assert.deepEqual(f.starts[0].keyframes.map(frame => frame.offset), [0, 1 / 6, 2 / 3, 5 / 6, 1]);
  assert.deepEqual(f.starts[0].keyframes.map(frame => frame.value), ['0', 0.4, 0.4, 0.4, 1]);
  assert.equal(f.starts[0].keyframes[3].easing, 'ease-in');
});

test('Anime timeline maps millisecond labels and previous-child anchors into one native batch', async () => {
  const f = await fixture();
  const group = f.module.adaptAnimeTimeline([
    { label: 'intro', position: 100 },
    { targets: 1, params: { opacity: 0.4, duration: 200, ease: 'linear' }, position: 'intro' },
    { targets: 1, params: { opacity: 0.8, duration: 100, ease: 'linear' }, position: '<' },
    { targets: 2, params: { opacity: 1, duration: 200, ease: 'linear' }, position: '<<' },
  ]);
  assert.equal(f.snapshotCalls.length, 1);
  assert.equal(JSON.stringify(f.snapshotCalls[0].targets), JSON.stringify([1, 1, 2]));
  assert.equal(f.batchStarts.length, 1);
  assert.equal(f.batchStarts[0].length, 2);
  assert.equal(group.animations.length, 2);
  const node1 = f.starts.find(start => start.node === 1);
  const node2 = f.starts.find(start => start.node === 2);
  assert.equal(node1.options.delay, 0.1);
  assert.equal(node1.options.duration, 0.3);
  assert.deepEqual(node1.keyframes.map(frame => frame.offset), [0, 0.666666666667, 1]);
  assert.deepEqual(node1.keyframes.map(frame => frame.value), ['0', 0.4, 0.8]);
  assert.equal(node2.options.delay, 0.3);
  assert.equal(node2.options.duration, 0.2);
});

test('Anime timeline defaults and relative label positions compile gaps into hold keyframes', async () => {
  const f = await fixture();
  f.module.adaptAnimeTimeline([
    { label: 'phase', position: 100 },
    { targets: 1, params: { opacity: 0.4 }, position: 'phase' },
    { targets: 1, params: { opacity: 0.8 }, position: 'phase+=300' },
    { targets: 2, params: { opacity: 1 }, position: 'phase*=2' },
  ], { defaults: { duration: 100, ease: 'linear' }, autoplay: false });
  assert.equal(f.starts.length, 2);
  const node1 = f.starts.find(start => start.node === 1);
  const node2 = f.starts.find(start => start.node === 2);
  assert.equal(node1.options.delay, 0.1);
  assert.equal(node1.options.duration, 0.4);
  assert.deepEqual(node1.keyframes.map(frame => frame.offset), [0, 0.25, 0.75, 1]);
  assert.deepEqual(node1.keyframes.map(frame => frame.value), ['0', 0.4, 0.4, 0.8]);
  assert.equal(node2.options.delay, 0.2);
  assert.equal(node2.options.duration, 0.1);
  assert.ok(f.controls.every(control => control.command === 'pause'));
});

test('Anime timeline set compiles an exact discontinuity without extending duration', async () => {
  const f = await fixture();
  f.module.adaptAnimeTimeline([
    { targets: 1, params: { opacity: 0.4, duration: 100, ease: 'linear' }, position: 0 },
    { targets: 1, set: { opacity: 0.8 }, position: 100 },
    { targets: 1, params: { opacity: 1, duration: 100, ease: 'linear' }, position: 100 },
  ]);
  assert.equal(f.snapshotCalls.length, 1);
  assert.equal(f.batchStarts.length, 1);
  assert.equal(f.starts.length, 1);
  assert.equal(f.starts[0].options.duration, 0.2);
  assert.equal(f.starts[0].options.fallback, undefined);
  assert.deepEqual(f.starts[0].keyframes.map(frame => frame.offset), [0, 0.5, 0.5, 1]);
  assert.deepEqual(f.starts[0].keyframes.map(frame => frame.value), ['0', 0.4, 0.8, 1]);
});

test('Anime timeline replacement truncates a linear track and holds the replacement result', async () => {
  const f = await fixture();
  f.module.adaptAnimeTimeline([
    { targets: 1, params: { opacity: 1, duration: 1000, ease: 'linear' }, position: 0 },
    { targets: 1, params: { opacity: 0, duration: 250, ease: 'linear' }, position: 500 },
  ]);
  assert.equal(f.batchStarts.length, 1);
  assert.equal(f.starts.length, 1);
  assert.equal(f.starts[0].options.delay, 0);
  assert.equal(f.starts[0].options.duration, 1);
  assert.deepEqual(f.starts[0].keyframes.map(frame => frame.offset), [0, 0.5, 0.75, 1]);
  assert.deepEqual(f.starts[0].keyframes.map(frame => frame.value), ['0', '0.5', 0, 0]);
});

test('Anime timeline replacement preserves an explicit boundary jump', async () => {
  const f = await fixture();
  f.module.adaptAnimeTimeline([
    { targets: 1, params: { opacity: 1, duration: 1000, ease: 'linear' }, position: 0 },
    { targets: 1, params: { opacity: [0.2, 0.8], duration: 250, ease: 'linear' }, position: 500 },
  ]);
  assert.equal(f.starts[0].options.duration, 1);
  assert.deepEqual(f.starts[0].keyframes.map(frame => frame.offset), [0, 0.5, 0.5, 0.75, 1]);
  assert.deepEqual(f.starts[0].keyframes.map(frame => frame.value), ['0', '0.5', 0.2, 0.8, 0.8]);
});

test('Anime timeline set replaces an active track and holds through its original end', async () => {
  const f = await fixture();
  f.module.adaptAnimeTimeline([
    { targets: 1, params: { opacity: 1, duration: 1000, ease: 'linear' }, position: 0 },
    { targets: 1, set: { opacity: 0.75 }, position: 500 },
  ]);
  assert.equal(f.starts[0].options.duration, 1);
  assert.deepEqual(f.starts[0].keyframes.map(frame => frame.offset), [0, 0.5, 0.5, 1]);
  assert.deepEqual(f.starts[0].keyframes.map(frame => frame.value), ['0', '0.5', 0.75, 0.75]);
});

test('Anime timeline replacement slices a cubic-bezier prefix without changing its curve', async () => {
  const f = await fixture();
  f.module.adaptAnimeTimeline([
    { targets: 1, params: { opacity: 1, duration: 1000, ease: 'ease-in' }, position: 0 },
    { targets: 1, params: { opacity: 0, duration: 250, ease: 'linear' }, position: 500 },
  ]);
  const frames = f.starts[0].keyframes;
  const original = [0.42, 0, 1, 1];
  const clipped = parseCubicBezier(frames[0].easing);
  const boundary = cubicBezierValue(original, 0.5);
  assert.ok(Math.abs(Number(frames[1].value) - boundary) < 1e-11);
  for (const local of [0.25, 0.5, 0.75]) {
    const expected = cubicBezierValue(original, local * 0.5) / boundary;
    assert.ok(Math.abs(cubicBezierValue(clipped, local) - expected) < 1e-10);
  }
  assert.deepEqual(frames.map(frame => frame.offset), [0, 0.5, 0.75, 1]);
  assert.deepEqual(frames.slice(2).map(frame => frame.value), [0, 0]);
});

test('Anime timeline replacement splits an exact quadratic in-out prefix at its midpoint', async () => {
  const f = await fixture();
  f.module.adaptAnimeTimeline([
    { targets: 1, params: { opacity: 1, duration: 1000, ease: 'inOut(2)' }, position: 0 },
    { targets: 1, params: { opacity: 0, duration: 250, ease: 'linear' }, position: 750 },
  ]);
  const frames = f.starts[0].keyframes;
  assert.deepEqual(frames.map(frame => frame.offset), [0, 0.5, 0.75, 1]);
  assert.deepEqual(frames.map(frame => Number(frame.value)), [0, 0.5, 0.875, 0]);
  assert.ok(frames[0].easing.startsWith('cubic-bezier('));
  assert.ok(frames[1].easing.startsWith('cubic-bezier('));
});

test('exact Anime power easing is canonicalized before native batch start', async () => {
  const f = await fixture();
  f.module.adaptAnimeJs(1, { opacity: 1, duration: 100, ease: 'inOut(3)' });
  const frames = f.starts[0].keyframes;
  assert.deepEqual(frames.map(frame => frame.offset), [0, 0.5, 1]);
  assert.deepEqual(frames.map(frame => Number(frame.value)), [0, 0.5, 1]);
  assert.ok(frames[0].easing.startsWith('cubic-bezier('));
  assert.ok(frames[1].easing.startsWith('cubic-bezier('));
});

test('Anime overlap keeps preceding exact power segments on the native easing subset', async () => {
  const f = await fixture();
  f.module.adaptAnimeTimeline([
    { targets: 1, params: { opacity: 0.4, duration: 400, ease: 'in(2)' }, position: 0 },
    { targets: 1, params: { opacity: 1, duration: 400, ease: 'in(2)' }, position: 400 },
    { targets: 1, params: { opacity: 0, duration: 100, ease: 'linear' }, position: 600 },
  ]);
  assert.equal(f.batchStarts.length, 1);
  assert.equal(f.starts.length, 1);
  assert.ok(f.starts[0].keyframes.some(frame => frame.easing?.startsWith('cubic-bezier(')));
  assert.ok(f.starts[0].keyframes.every(frame => !frame.easing?.startsWith('rml-power(')));
});

test('Anime timeline rejects a power prefix that has no exact cubic representation', async () => {
  const f = await fixture();
  assert.throws(
    () => f.module.adaptAnimeTimeline([
      { targets: 1, params: { opacity: 1, duration: 1000, ease: 'out(4)' }, position: 0 },
      { targets: 1, params: { opacity: 0, duration: 250, ease: 'linear' }, position: 500 },
    ]),
    error => error.code === 'unsupported_overlap_easing' && error.library === 'animejs@4.5.0',
  );
  assert.equal(f.batchStarts.length, 0);
});

test('Anime timeline rejects unknown labels and child or parent playback semantics', async () => {
  const unknown = await fixture();
  assert.throws(
    () => unknown.module.adaptAnimeTimeline([
      { targets: 1, params: { opacity: 1, duration: 100 }, position: 'missing' },
    ]),
    error => error.code === 'unknown_timeline_label' && error.library === 'animejs@4.5.0',
  );
  assert.equal(unknown.batchStarts.length, 0);

  const childCallback = await fixture();
  assert.throws(
    () => childCallback.module.adaptAnimeTimeline([
      { targets: 1, params: { opacity: 1, duration: 100, onComplete() {} } },
    ]),
    error => error.code === 'unsupported_timeline_child_callback',
  );

  const parentPlayback = await fixture();
  assert.throws(
    () => parentPlayback.module.adaptAnimeTimeline([
      { targets: 1, params: { opacity: 1, duration: 100 } },
    ], { loop: 1 }),
    error => error.code === 'unsupported_timeline_playback',
  );

  const positionFunction = await fixture();
  assert.throws(
    () => positionFunction.module.adaptAnimeTimeline([
      { targets: '.items', params: { opacity: 1, duration: 100 }, position: () => 0 },
    ]),
    error => error.code === 'unsupported_timeline_position_function',
  );
});

test('Anime timeline position stagger expands targets before advancing timeline end', async () => {
  const f = await fixture();
  const stagger = f.module.animeTimelinePositionStagger(50, { start: 'intro', from: 'last' });
  f.module.adaptAnimeTimeline([
    { label: 'intro', position: 100 },
    { targets: '.items', position: stagger, params: { opacity: 0.5, duration: 100, ease: 'linear' } },
    { targets: 1, params: { scale: 1.1, duration: 50, ease: 'linear' } },
  ]);
  assert.equal(f.snapshotCalls.length, 1);
  assert.equal(f.batchStarts.length, 1);
  const node1Opacity = f.starts.find(start => start.node === 1 && start.property === 'opacity');
  const node2Opacity = f.starts.find(start => start.node === 2 && start.property === 'opacity');
  const transform = f.starts.find(start => start.property === 'transform');
  assert.equal(node1Opacity.options.delay, 0.15);
  assert.equal(node2Opacity.options.delay, 0.1);
  assert.equal(transform.options.delay, 0.25);
});

test('Anime previous-child anchor follows the last expanded position-stagger child', async () => {
  const f = await fixture();
  f.module.adaptAnimeTimeline([
    { targets: '.items', position: f.module.animeTimelinePositionStagger(50, { from: 'last' }),
      params: { opacity: 0.5, duration: 100, ease: 'linear' } },
    { targets: 1, position: '<', params: { scale: 1.1, duration: 50, ease: 'linear' } },
  ]);
  const transform = f.starts.find(start => start.property === 'transform');
  assert.equal(transform.options.delay, 0.1);
});

test('GSAP stagger expands to target delays and property arrays become keyframes', async () => {
  const f = await fixture();
  f.module.adaptGsapTo('.items', {
    duration: 2,
    stagger: { each: 0.2, from: 'end' },
    keyframes: { opacity: [0, 0.25, 1], ease: 'none', easeEach: 'none' },
  });
  assert.equal(f.snapshotCalls.length, 1);
  assert.equal(f.batchStarts.length, 1);
  assert.equal(f.batchStarts[0].length, 2);
  assert.equal(f.starts.length, 2);
  assert.deepEqual(f.starts.map(start => start.options.delay), [0.2, 0]);
  assert.deepEqual(f.starts[0].keyframes.map(frame => frame.offset), [0, 0.5, 1]);
  assert.deepEqual(f.starts[0].keyframes.map(frame => frame.value), [0, 0.25, 1]);
});

test('GSAP sequential keyframes scale segment timing and preserve delay holds', async () => {
  const f = await fixture();
  f.module.adaptGsapTo('.items', {
    duration: 0.9, ease: 'none', stagger: 0.1,
    keyframes: [
      { opacity: 0.4, duration: 0.1, ease: 'none' },
      { opacity: '+=0.2', delay: 0.05, duration: 0.3, ease: 'none' },
    ],
  });
  assert.equal(f.snapshotCalls.length, 1);
  assert.deepEqual([...f.snapshotCalls[0].properties], ['opacity']);
  assert.equal(f.starts.length, 2);
  assert.deepEqual(f.starts.map(start => start.options.duration), [0.9, 0.9]);
  assert.deepEqual(f.starts.map(start => start.options.delay), [0, 0.1]);
  const offsets = f.starts[0].keyframes.map(frame => frame.offset);
  assert.ok(offsets.every((offset, index) => Math.abs(offset - [0, 2 / 9, 1 / 3, 1][index]) < 1e-12));
  assert.deepEqual(f.starts[0].keyframes.map(frame => frame.value), ['0', 0.4, 0.4, 0.6]);
  assert.deepEqual(f.starts[1].keyframes.map(frame => frame.value), ['0.25', 0.4, 0.4, 0.6]);
});

test('GSAP timeline resolves labels and anchors, merges sequential properties and starts one native batch', async () => {
  const f = await fixture();
  const group = f.module.adaptGsapTimeline([
    { label: 'intro', position: 0.2 },
    { targets: 1, vars: { opacity: 0.5, duration: 0.3, ease: 'none' }, position: 'intro' },
    { targets: 1, vars: { opacity: 1, duration: 0.2, ease: 'none' }, position: '>' },
    { targets: 2, vars: { opacity: 1, duration: 0.4, ease: 'none' }, position: '<' },
  ]);
  assert.equal(f.snapshotCalls.length, 1);
  assert.equal(JSON.stringify(f.snapshotCalls[0].targets), JSON.stringify([1, 1, 2]));
  assert.equal(f.batchStarts.length, 1);
  assert.equal(f.batchStarts[0].length, 2);
  assert.equal(group.animations.length, 2);
  const node1 = f.starts.find(start => start.node === 1);
  const node2 = f.starts.find(start => start.node === 2);
  assert.equal(node1.options.delay, 0.2);
  assert.equal(node1.options.duration, 0.5);
  assert.deepEqual(node1.keyframes.map(frame => frame.offset), [0, 0.6, 1]);
  assert.deepEqual(node1.keyframes.map(frame => frame.value), ['0', 0.5, 1]);
  assert.equal(node2.options.delay, 0.5);
  assert.equal(node2.options.duration, 0.4);
});

test('GSAP timeline label offsets compile gaps into hold keyframes', async () => {
  const f = await fixture();
  f.module.adaptGsapTimeline([
    { label: 'phase', position: 0.1 },
    { targets: 1, vars: { opacity: 0.4, duration: 0.2, ease: 'none' }, position: 'phase' },
    { targets: 1, vars: { opacity: 0.8, duration: 0.2, ease: 'none' }, position: 'phase+=0.5' },
  ]);
  assert.equal(f.starts.length, 1);
  assert.equal(f.starts[0].options.delay, 0.1);
  assert.equal(f.starts[0].options.duration, 0.7);
  assert.deepEqual(f.starts[0].keyframes.map(frame => frame.offset), [0, 0.285714285714, 0.714285714286, 1]);
  assert.deepEqual(f.starts[0].keyframes.map(frame => frame.value), ['0', 0.4, 0.4, 0.8]);
  assert.equal(f.starts[0].keyframes[1].easing, 'linear');
});

test('GSAP timeline set stays zero-duration and rejects a JS fallback route', async () => {
  const f = await fixture();
  f.module.adaptGsapTimeline([
    { targets: 1, set: { opacity: 0.75 }, position: 0.25 },
  ]);
  assert.equal(f.starts.length, 1);
  assert.equal(f.starts[0].options.delay, 0.25);
  assert.equal(f.starts[0].options.duration, 0);
  assert.equal(f.starts[0].options.fallback, undefined);
  assert.deepEqual(f.starts[0].keyframes.map(frame => frame.offset), [0, 1]);
  assert.deepEqual(f.starts[0].keyframes.map(frame => frame.value), ['0', 0.75]);

  const unsupported = await fixture();
  assert.throws(
    () => unsupported.module.adaptGsapTimeline([
      { targets: 1, set: { marginLeft: 20 }, position: 0 },
    ]),
    /unsupported_property/,
  );
  assert.equal(unsupported.frames.size, 0);
});

test('same-time timeline sets collapse to one zero-duration last-write result', async () => {
  const f = await fixture();
  f.module.adaptGsapTimeline([
    { targets: 1, set: { opacity: 0.25 }, position: 0.1 },
    { targets: 1, set: { opacity: 0.75 }, position: 0.1 },
  ]);
  assert.equal(f.starts.length, 1);
  assert.equal(f.starts[0].options.delay, 0.1);
  assert.equal(f.starts[0].options.duration, 0);
  assert.deepEqual(f.starts[0].keyframes.map(frame => frame.offset), [0, 1]);
  assert.deepEqual(f.starts[0].keyframes.map(frame => frame.value), ['0', 0.75]);
});

test('timeline set rejects library options and empty property objects', async () => {
  const anime = await fixture();
  assert.throws(
    () => anime.module.adaptAnimeTimeline([{ targets: 1, set: { duration: 100 } }]),
    error => error.code === 'unsupported_set_option',
  );
  const gsap = await fixture();
  assert.throws(
    () => gsap.module.adaptGsapTimeline([{ targets: 1, set: {} }]),
    error => error.code === 'missing_properties',
  );
});

test('GSAP timeline anchors advance past the latest staggered target', async () => {
  const f = await fixture();
  f.module.adaptGsapTimeline([
    { targets: '.items', position: 0, vars: { opacity: 0.5, duration: 0.2, stagger: 0.1, ease: 'none' } },
    { label: 'after-stagger' },
    { targets: 1, position: '>', vars: { scale: 1.1, duration: 0.1, ease: 'none' } },
    { targets: 2, position: 'after-stagger', vars: { scale: 1.2, duration: 0.1, ease: 'none' } },
  ]);
  assert.equal(f.snapshotCalls.length, 1);
  assert.equal(f.batchStarts.length, 1);
  const node1Opacity = f.starts.find(start => start.node === 1 && start.property === 'opacity');
  const node2Opacity = f.starts.find(start => start.node === 2 && start.property === 'opacity');
  const transforms = f.starts.filter(start => start.property === 'transform');
  assert.equal(node1Opacity.options.delay, 0);
  assert.equal(node2Opacity.options.delay, 0.1);
  assert.equal(node1Opacity.options.duration, 0.2);
  assert.equal(node2Opacity.options.duration, 0.2);
  assert.deepEqual(transforms.map(start => start.options.delay), [0.3, 0.3]);
});

test('GSAP timeline end includes reverse stagger regardless of target order', async () => {
  const f = await fixture();
  f.module.adaptGsapTimeline([
    { targets: '.items', position: 0, vars: {
      opacity: 0.5, duration: 0.2, stagger: { each: 0.1, from: 'end' }, ease: 'none',
    } },
    { targets: 2, vars: { scale: 1.2, duration: 0.1, ease: 'none' } },
  ]);
  const node1Opacity = f.starts.find(start => start.node === 1 && start.property === 'opacity');
  const node2Opacity = f.starts.find(start => start.node === 2 && start.property === 'opacity');
  const transform = f.starts.find(start => start.property === 'transform');
  assert.equal(node1Opacity.options.delay, 0.1);
  assert.equal(node2Opacity.options.delay, 0);
  assert.equal(transform.options.delay, 0.3);
});

test('GSAP timeline maps overlapping writes to ordered native contributions', async () => {
  const f = await fixture();
  const group = f.module.adaptGsapTimeline([
    { targets: 1, vars: { opacity: 0.5, duration: 1, ease: 'none' } },
    { targets: 1, vars: { opacity: 1, duration: 0.2, ease: 'none' }, position: '<+=0.5' },
  ]);
  assert.equal(f.batchStarts.length, 1);
  assert.equal(f.batchStarts[0].length, 2);
  assert.equal(group.animations.length, 2);
  assert.deepEqual(f.starts.map(start => start.options.composite), ['layered-replace', 'layered-replace']);
  assert.deepEqual(f.starts.map(start => start.options.compositionOrder), [0, 1]);
  assert.deepEqual(f.starts.map(start => start.options.delay), [0, 0.5]);
  assert.deepEqual(f.starts.map(start => start.options.duration), [1, 0.2]);
  assert.deepEqual(f.starts[0].keyframes.map(frame => frame.value), ['0', 0.5]);
  assert.deepEqual(f.starts[1].keyframes.map(frame => frame.value), ['0.25', 1]);
});

test('GSAP sequential explicit from values remain exact discontinuities', async () => {
  const f = await fixture();
  f.module.adaptGsapTimeline([
    { targets: 1, vars: { opacity: 0.5, duration: 0.2, ease: 'none' } },
    { targets: 1, vars: { opacity: [0.2, 1], duration: 0.2, ease: 'none' }, position: '>' },
  ]);
  assert.equal(f.batchStarts.length, 1);
  assert.equal(f.starts.length, 1);
  assert.deepEqual(f.starts[0].keyframes.map(frame => frame.value), ['0', 0.5, 0.2, 1]);
});

test('GSAP fromTo maps seconds, repeat/yoyo and a single transform primitive', async () => {
  const f = await fixture();
  const group = f.module.adaptGsapFromTo([1, 2], { x: 0 }, {
    x: 100, duration: 2, repeat: 1, yoyo: true, ease: 'power1.out',
  });
  assert.equal(group.animations.length, 2);
  assert.equal(f.snapshotCalls.length, 1);
  assert.equal(f.starts[0].property, 'transform');
  assert.equal(f.starts[0].options.duration, 2);
  assert.equal(f.starts[0].options.iterations, 2);
  assert.equal(f.starts[0].options.direction, 'alternate');
  assert.equal(f.starts[0].keyframes[0].value, 'translate(0px,0px)');
  assert.equal(f.starts[0].keyframes[1].value, 'translate(100px,0px)');
  assert.equal(f.starts[0].keyframes[0].easing, 'cubic-bezier(0.333333333333,0.666666666667,0.666666666667,1)');
  assert.ok(group.animations.every(animation => animation.route === 'native'));
  for (const animation of group.animations) {
    f.native.OnAnimationEvent.emit(JSON.stringify({
      handle: animation.handle, reason: 'completed', state: 'finished',
    }));
  }
  const results = await group.finished;
  assert.equal(results.length, 2);
  assert.ok(results.every(result => result.reason === 'completed'));
});

test('GSAP affine aliases, autoAlpha and colors compile to native tracks', async () => {
  const f = await fixture();
  const affine = f.module.adaptGsapTo(1, {
    xPercent: 50, skewX: 12, skewY: -4, duration: 0.2, ease: 'none',
  });
  assert.equal(affine.animations.length, 1);
  assert.equal(f.starts.at(-1).property, 'transform');
  assert.equal(f.starts.at(-1).keyframes.at(-1).value,
    'translate(50px,0px) scale(1,1) rotate(0deg) skew(12deg,-4deg)');

  const alpha = f.module.adaptGsapTo(1, { autoAlpha: 0, duration: 0.2, ease: 'none' });
  assert.equal(alpha.animations.length, 2);
  assert.deepEqual(f.starts.slice(-2).map(entry => entry.property), ['opacity', 'visibility']);
  assert.equal(f.starts.at(-1).keyframes.at(-1).value, 'hidden');

  const color = f.module.adaptGsapTo(1, { backgroundColor: '#33669980', duration: 0.2, ease: 'none' });
  assert.equal(color.animations.length, 1);
  assert.equal(f.starts.at(-1).property, 'background-color');
  assert.equal(f.starts.at(-1).keyframes.at(-1).value, '#33669980');
});

test('adapters compose synchronized transforms and reject callback or mismatched timing semantics', async () => {
  const f = await fixture();
  assert.throws(
    () => f.module.adaptAnimationJs({ el: 1, draw: () => {}, dur: 100 }),
    error => error.code === 'unsupported_callback' && error.library === 'animationjs@0.5.0',
  );
  assert.throws(
    () => f.module.adaptAnimeJs(1, { opacity: 1, modifier: value => value }),
    error => error.code === 'unsupported_callback' && error.library === 'animejs@4.5.0',
  );
  assert.throws(
    () => f.module.adaptAnimeJs(1, { opacity: 1, priority: 10 }),
    error => error.code === 'unsupported_priority' && error.library === 'animejs@4.5.0',
  );
  const composed = f.module.adaptGsapTo(1, { x: 10, y: 20, duration: 0.1, ease: 'none' });
  assert.equal(composed.animations.length, 1);
  assert.equal(f.starts.at(-1).keyframes.at(-1).value,
    'translate(10px,20px) scale(1,1) rotate(0deg)');
  composed.cancel();
  assert.throws(() => f.module.compileAnimationSourceRequest({
    library: 'composition-test', targets: 1, defaultEasing: 'linear', options: { duration: 1 },
    tracks: [
      { sourceName: 'scale', frames: [{ offset: 0, value: 1 }, { offset: 1, value: 2 }] },
      { sourceName: 'rotate', frames: [
        { offset: 0, value: 0 }, { offset: 0.5, value: 45 }, { offset: 1, value: 90 },
      ] },
    ],
  }), error => error.code === 'unsupported_transform_timing_composition');
  assert.throws(
    () => f.module.adaptAnimeJs(1, { keyframes: [{ opacity: 1, duration: 0 }] }),
    error => error.code === 'invalid_timing' && error.library === 'animejs@4.5.0',
  );
  assert.throws(
    () => f.module.adaptGsapTo(1, { keyframes: [{ opacity: 1, onUpdate: () => {} }] }),
    error => error.code === 'unsupported_keyframe_option' && error.library === 'gsap@3.15.0',
  );
  assert.throws(
    () => f.module.adaptGsapTo([1, 2], { opacity: 1, stagger: { each: 0.1, grid: [2, 1] } }),
    error => error.code === 'unsupported_stagger_distribution' && error.library === 'gsap@3.15.0',
  );
});
