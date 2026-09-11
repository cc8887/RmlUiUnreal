import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import path from 'node:path';
import { validateCss, buildFrontend } from '../tools/build.mjs';

test('CSS diagnostics retain the implemented grid syntax', () => {
  assert.doesNotThrow(() => validateCss('.grid { display:grid; grid-template-columns:repeat(3,minmax(0px,1fr)); grid-template-areas:"a b c"; gap:16px; }', 'grid.css'));
  assert.throws(() => validateCss('.x { backdrop-filter:blur(3px); }', 'bad.css'), /Unsupported RmlUi CSS property/);
  assert.throws(() => validateCss('.x { color:var(--color); }', 'bad.css'), /CSS variables/);
});
test('actual SFC bundle has reproducible content-addressed versions and complete hashes', async () => {
  const first = await buildFrontend(), second = await buildFrontend();
  assert.equal(first.version, second.version);
  const manifest = JSON.parse(await readFile(path.join(first.directory, 'manifest.json'), 'utf8'));
  for (const [name, expected] of Object.entries(manifest.files)) {
    assert.match(name, /^[a-zA-Z0-9_.-]+(?:\/[a-zA-Z0-9_.-]+)*$/, 'manifest paths follow the native runtime contract');
    const data = await readFile(path.join(first.directory, name));
    assert.equal(createHash('sha256').update(data).digest('hex'), expected, name);
  }
  const source = await readFile(path.join(first.directory, 'app.js'), 'utf8');
  assert.ok(source.includes('createRenderer'));
  assert.ok(!source.includes('react-dom'));
  assert.ok(!source.includes('document.createElement'));
});
test('actor observer compiles Tailwind utilities into the supported RmlUi CSS subset', async () => {
  const result = await buildFrontend({ actors: true });
  const css = await readFile(path.join(result.directory, 'app.rcss'), 'utf8');
  const source = await readFile(path.join(result.directory, 'app.js'), 'utf8');
  assert.match(css, /\.grid-cols-3/);
  assert.match(css, /minmax\(0px,\s*1fr\)/);
  assert.ok(!css.includes('var(--tw-'));
  assert.match(css, /grid-template-areas:\s*"icons image"/);
  assert.match(css, /box-shadow:/);
  assert.match(css, /@keyframes scan-line/);
  assert.ok(source.includes('GetActorSnapshot'));
  assert.ok(source.includes('GetActorDetails'));
  assert.ok(source.includes('hello_world.png'));
  assert.ok(source.includes('Pause live refresh'));
});
