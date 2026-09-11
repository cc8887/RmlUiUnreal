import assert from 'node:assert/strict';
import { mkdtemp, mkdir, readFile, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import test from 'node:test';
import { compileDocumentFile, compileDocumentMarkup } from '../src/compile-document.mjs';

test('compiles an in-memory LLM document with the shared CSS rules', () => {
  const source = '<html><head><style>.magictime { animation-duration: 1s; } .puffIn { animation-name: puffIn; } @keyframes puffIn { from { opacity: 0; } to { opacity: 1; } }</style></head><body><div class="magictime puffIn"/></body></html>';
  const result = compileDocumentMarkup(source, { from: 'llm-response.html' });
  assert.match(result.markup, /\[class~="puffIn"\]\s*\{\s*animation: 1s cubic-out puffIn/);
  assert.equal(result.diagnostics.some((item) => item.code === 'camel-case-selector-broadened'), true);
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
});
