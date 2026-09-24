import assert from 'node:assert/strict';
import { mkdtemp, mkdir, readFile, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import test from 'node:test';
import { compileDocumentFile, compileDocumentMarkup } from '../src/compile-document.mjs';

test('compiles an in-memory LLM document with the shared CSS rules', () => {
  const source = '<html><head><style>.magictime { animation-duration: 1s; } .puffIn { animation-name: puffIn; } @keyframes puffIn { from { opacity: 0; } to { opacity: 1; } }</style></head><body><div class="magictime puffIn"/></body></html>';
  const result = compileDocumentMarkup(source, { from: 'llm-response.html' });
  assert.doesNotMatch(result.markup, /@keyframes|animation:/);
  assert.equal(result.motionManifest.rules.length, 1);
  assert.equal(result.motionManifest.rules[0].selector, '.magictime[class~="puffIn"]');
  assert.equal(result.diagnostics.some((item) => item.severity === 'error'), false);
});

test('compiles linked and inline CSS and emits a content manifest', async () => {
  const root = await mkdtemp(path.join(tmpdir(), 'rmlui-webcompat-'));
  const inputDir = path.join(root, 'input');
  const outputDir = path.join(root, 'output');
  await mkdir(inputDir);
  await writeFile(path.join(inputDir, 'motion.css'), '.grow { transition-duration: .3s; transition-property: transform; }');
  await writeFile(path.join(inputDir, 'motion.html'), '<html><head><link rel="stylesheet" href="motion.css"/><style>.fx { animation-duration: 1s; animation-name: puff; }</style></head><body/></html>');

  const result = await compileDocumentFile(path.join(inputDir, 'motion.html'), path.join(outputDir, 'motion.html'));
  const html = await readFile(path.join(outputDir, 'motion.html'), 'utf8');
  const css = await readFile(path.join(outputDir, 'motion.css'), 'utf8');
  const manifest = JSON.parse(await readFile(result.manifestPath, 'utf8'));
  assert.match(html, /animation: 1s cubic-out puff/);
  assert.match(css, /transition: transform \.3s cubic-out/);
  assert.equal(manifest.schemaVersion, 1);
  assert.equal(manifest.emittedFiles.length, 2);
  assert.deepEqual(manifest.motionManifest, { schemaVersion: 1, rules: [] });
});

test('retains animation play-state from inline and linked stylesheets', async () => {
  const source = '<html><head><style>.paused { animation-play-state: paused; }</style></head><body/></html>';
  assert.deepEqual(compileDocumentMarkup(source).motionManifest.playStates,
    [{ selector: '.paused', paused: true }]);
  const root = await mkdtemp(path.join(tmpdir(), 'rmlui-webcompat-play-state-'));
  const inputDir = path.join(root, 'input');
  const outputDir = path.join(root, 'output');
  await mkdir(inputDir);
  await writeFile(path.join(inputDir, 'motion.css'), '.running { animation-play-state: running; }');
  await writeFile(path.join(inputDir, 'motion.html'),
    '<html><head><link rel="stylesheet" href="motion.css"/><style>.paused { animation-play-state: paused; }</style></head><body/></html>');
  const result = await compileDocumentFile(path.join(inputDir, 'motion.html'), path.join(outputDir, 'motion.html'));
  assert.deepEqual(result.motionManifest.playStates, [
    { selector: '.running', paused: false }, { selector: '.paused', paused: true },
  ]);
  const persisted = JSON.parse(await readFile(result.manifestPath, 'utf8'));
  assert.deepEqual(persisted.motionManifest.playStates, result.motionManifest.playStates);
});

test('preserves document stylesheet order for play-state overrides', async () => {
  const root = await mkdtemp(path.join(tmpdir(), 'rmlui-webcompat-order-'));
  const inputDir = path.join(root, 'input');
  const outputDir = path.join(root, 'output');
  await mkdir(inputDir);
  await writeFile(path.join(inputDir, 'state.css'), '.motion { animation-play-state: running; }');
  await writeFile(path.join(inputDir, 'state.html'),
    '<html><head><link rel="stylesheet" href="state.css"/><style>.motion { animation-play-state: paused; }</style></head><body/></html>');
  const result = await compileDocumentFile(path.join(inputDir, 'state.html'), path.join(outputDir, 'state.html'));
  assert.deepEqual(result.motionManifest.playStates, [
    { selector: '.motion', paused: false }, { selector: '.motion', paused: true },
  ]);
});

test('reports when keyframes and animation rules split across style blocks', () => {
  const result = compileDocumentMarkup('<html><head><style>@keyframes fade { from { opacity:0; } to { opacity:1; } }</style>' +
    '<style>.motion { animation-name:fade; animation-duration:1s; }</style></head><body/></html>');
  assert.equal(result.motionManifest.rules.length, 0);
  assert.match(result.markup, /animation:/);
  assert.ok(result.diagnostics.some((item) => item.code === 'native-keyframes-not-in-source' && item.severity === 'warning'));
});
