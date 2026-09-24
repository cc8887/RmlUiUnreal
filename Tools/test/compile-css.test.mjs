import assert from 'node:assert/strict';
import test from 'node:test';
import { compileCss } from '../src/compile-css.mjs';

test('composes Magic.css animation longhands across classes', () => {
  const source = `.magictime {
    -webkit-animation-duration: 1s;
    animation-duration: 1s;
    -webkit-animation-fill-mode: both;
    animation-fill-mode: both;
  }
  .puffIn {
    -webkit-animation-name: puffIn;
    animation-name: puffIn;
  }`;
  const result = compileCss(source, { from: 'magic.css' });
  assert.match(result.css, /\[class~="puffIn"\]\s*\{\s*animation: 1s cubic-out puffIn/);
  assert.doesNotMatch(result.css, /(?:^|\s)animation-duration:/m);
  assert.equal(result.diagnostics.filter((item) => item.code === 'animation-fill-mode-dropped').length, 1);
  assert.equal(result.diagnostics.filter((item) => item.code === 'camel-case-selector-broadened').length, 1);
  assert.equal(result.diagnostics.filter((item) => item.severity === 'error').length, 0);
});

test('compiles CSS keyframes to the native motion manifest', () => {
  const source = `.motion { animation-duration: 240ms; animation-delay: 20ms; animation-timing-function: steps(4, jump-both); animation-fill-mode: both; }
    .fadeIn { animation-name: fadeIn; animation-direction: alternate-reverse; animation-iteration-count: 2; }
    @keyframes fadeIn {
      from { opacity: 0; transform: translateY(12px) scale(.9); }
      50% { opacity: .6; transform: translateY(2px) scale(1.03); animation-timing-function: cubic-bezier(.2, 1.2, .4, 1); }
      to { opacity: 1; transform: translateY(0px) scale(1); }
    }`;
  const result = compileCss(source, { from: 'motion.css' });
  assert.doesNotMatch(result.css, /@keyframes|animation:/);
  assert.equal(result.motionManifest.rules.length, 1);
  const rule = result.motionManifest.rules[0];
  assert.equal(rule.selector, '.motion[class~="fadeIn"]');
  assert.deepEqual({ duration: rule.duration, delay: rule.delay, iterations: rule.iterations, direction: rule.direction, fill: rule.fill },
    { duration: 0.24, delay: 0.02, iterations: 2, direction: 3, fill: 3 });
  assert.deepEqual(rule.tracks.map((track) => track.property), ['opacity', 'transform']);
  assert.deepEqual(rule.tracks[0].keyframes[0].easing, [2, 4, 3, 0, 0]);
  assert.deepEqual(rule.tracks[0].keyframes.map((frame) => frame.values), [[0], [.6], [1]]);
  assert.deepEqual(rule.tracks[0].keyframes[1].easing, [1, .2, 1.2, .4, 1]);
  assert.deepEqual(rule.tracks[1].keyframes[0].values, [0, 12, .9, .9, 0, 0, 0]);
  assert.equal(result.diagnostics.filter((item) => item.severity === 'error').length, 0);
});

test('preserves infinite iterations and negative delay in the native motion manifest', () => {
  const source = `.pulse { animation-name: pulse; animation-duration: 2s; animation-delay: -2.5s;
    animation-iteration-count: infinite; animation-direction: alternate; }
    @keyframes pulse { from { opacity: 0; } to { opacity: 1; } }`;
  const result = compileCss(source, { from: 'motion.css' });
  assert.equal(result.diagnostics.filter((item) => item.severity === 'error').length, 0);
  assert.equal(result.motionManifest.rules.length, 1);
  assert.deepEqual({ delay: result.motionManifest.rules[0].delay, iterations: result.motionManifest.rules[0].iterations },
    { delay: -2.5, iterations: 0 });
});

test('emits mutation-time animation play-state rules', () => {
  const result = compileCss(`.motion { animation-name: pulse; animation-duration: 2s; animation-play-state: running; }
    .paused { animation-play-state: paused; }
    @keyframes pulse { from { opacity: 0; } to { opacity: 1; } }`);
  assert.deepEqual(result.motionManifest.playStates, [
    { selector: '.motion', paused: false }, { selector: '.paused', paused: true },
  ]);
});

test('rejects native selectors whose state changes cannot be observed', () => {
  const keyframes = '@keyframes fade { from { opacity: 0; } to { opacity: 1; } }';
  for (const selector of ['.motion:hover', '.open + .motion', '.open ~ .motion']) {
    const result = compileCss(`${selector} { animation-name: fade; animation-duration: 1s; } ${keyframes}`);
    assert.equal(result.motionManifest.rules.length, 0, selector);
    assert.ok(result.diagnostics.some((item) => item.code === 'unsupported-native-animation-selector' && item.severity === 'error'), selector);
  }
  const descendant = compileCss('.open .motion { animation-name: fade; animation-duration: 1s; } ' + keyframes);
  assert.deepEqual(descendant.motionManifest.rules.map((rule) => rule.selector), ['.open .motion']);
  const stateOnly = compileCss('.motion:hover { animation-play-state: paused; }');
  assert.ok(stateOnly.diagnostics.some((item) => item.code === 'unsupported-native-animation-selector' && item.severity === 'error'));
});

test('rejects keyframes that need an unsupported native property', () => {
  const result = compileCss('.x { animation-duration: 1s; animation-name: blur; } @keyframes blur { from { filter: blur(0); } to { filter: blur(4px); } }');
  assert.equal(result.diagnostics.some((item) => item.code === 'unsupported-native-css-animation' && item.severity === 'error'), true);
});

test('keeps a shared duration provider for a sibling without local keyframes', () => {
  const result = compileCss('.motion { animation-duration: 1s; } .native { animation-name: fade; } .external { animation-name: external; } @keyframes fade { from { opacity: 0; } to { opacity: 1; } }');
  assert.equal(result.motionManifest.rules.length, 1);
  assert.match(result.css, /\.motion\.external\s*\{\s*animation: 1s cubic-out external/);
});

test('converts Hover.css transition longhands and vendor duplicates', () => {
  const source = `.hvr-grow {
    -webkit-transform: perspective(1px) translateZ(0);
    transform: perspective(1px) translateZ(0);
    -webkit-transition-duration: 0.3s;
    transition-duration: 0.3s;
    -webkit-transition-property: transform;
    transition-property: transform;
  }`;
  const result = compileCss(source, { from: 'hover.css' });
  assert.doesNotMatch(result.css, /transform: perspective/);
  assert.match(result.css, /transform: scale\(1\)/);
  assert.match(result.css, /transition: transform 0.3s cubic-out/);
  assert.doesNotMatch(result.css, /(?:^|\s)transition-duration:/m);
  assert.equal(result.diagnostics.filter((item) => item.severity === 'error').length, 0);
  assert.equal(result.diagnostics.some((item) => item.code === 'gpu-promotion-transform-normalized'), true);
});

test('fails explicitly for timing functions without an RmlUi equivalent', () => {
  const result = compileCss('.x { transition-property: transform; transition-timing-function: cubic-bezier(0, 1, 1, 0); }');
  assert.equal(result.diagnostics.some((item) => item.code === 'unsupported-timing-function' && item.severity === 'error'), true);
});

test('removes duplicated prefixed keyframes', () => {
  const result = compileCss('@-webkit-keyframes puffIn { from { opacity: 0; } } @keyframes puffIn { from { opacity: 0; } }');
  assert.doesNotMatch(result.css, /@-webkit-keyframes/);
  assert.equal((result.css.match(/@keyframes/g) ?? []).length, 1);
});

test('lowers a registered Unreal UI material binding to the internal decorator', () => {
  const result = compileCss('.panel { background: rgba(0,0,0,.4); -rmlui-material: panel.energy; -rmlui-material-slot: background; }');
  assert.match(result.css, /background: rgba\(0,0,0,\.4\)/);
  assert.match(result.css, /decorator: ue-material\(panel\.energy\)/);
  assert.doesNotMatch(result.css, /-rmlui-material/);
  assert.equal(result.diagnostics.filter((item) => item.severity === 'error').length, 0);
});

test('lowers a border Unreal UI material to a slot-aware decorator', () => {
  const result = compileCss('.panel { border: 8px solid white; -rmlui-material: panel.frame; -rmlui-material-slot: border; }');
  assert.match(result.css, /decorator: ue-material-border\(panel\.frame\)/);
  assert.match(result.css, /border: 8px white/);
  assert.doesNotMatch(result.css, /\bsolid\b/);
  assert.equal(result.diagnostics.filter((item) => item.severity === 'error').length, 0);
});

test('rejects non-solid Web border styles that cannot be represented', () => {
  const result = compileCss('.panel { border: 8px dashed white; }');
  assert.equal(result.diagnostics.some((item) => item.code === 'unsupported-border-style' && item.severity === 'error'), true);
});

test('rejects the reserved foreground Unreal UI material slot explicitly', () => {
  const result = compileCss('.panel { -rmlui-material: panel.glow; -rmlui-material-slot: foreground; }');
  assert.equal(result.diagnostics.some((item) => item.code === 'unsupported-unreal-material-slot' && item.severity === 'error'), true);
  assert.doesNotMatch(result.css, /decorator:/);
});

test('rejects unknown Unreal UI material slots', () => {
  const result = compileCss('.panel { -rmlui-material: panel.glow; -rmlui-material-slot: overlay; }');
  assert.equal(result.diagnostics.some((item) => item.code === 'invalid-unreal-material-slot' && item.severity === 'error'), true);
  assert.doesNotMatch(result.css, /decorator:/);
});

test('rejects Unreal asset paths in CSS material bindings', () => {
  const result = compileCss('.panel { -rmlui-material: /Game/UI/M_Frosted; }');
  assert.equal(result.diagnostics.some((item) => item.code === 'invalid-unreal-material-alias' && item.severity === 'error'), true);
  assert.doesNotMatch(result.css, /decorator:/);
});
