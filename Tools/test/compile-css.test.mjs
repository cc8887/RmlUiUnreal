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
