import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile, readdir } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const plugin = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const read = relative => readFile(path.join(plugin, relative), 'utf8');

test('core plugin builds without example modules or staged example bundles', async () => {
  const descriptor = JSON.parse(await read('RmlUiUnreal.uplugin'));
  assert.ok(descriptor.Plugins.some(item => item.Name === 'Puerts' && item.Enabled));
  assert.ok(descriptor.Modules.every(item => !item.Name.includes('Samples') && item.Name !== 'RmlUiUnrealJSEditor'));
  const rules = await read('Source/RmlUiUnrealJS/RmlUiUnrealJS.Build.cs');
  assert.doesNotMatch(rules, /ActorObserver|Content.*Chat|Content.*Vue|RmlUiUnrealSamples/);
  const sources = await readdir(path.join(plugin, 'Source/RmlUiUnrealJS/Public'));
  assert.ok(!sources.includes('RmlUiActorObserverService.h'));
  assert.ok(!sources.includes('RmlUiChatTransport.h'));
});

test('example libraries belong to the separate sample dependency graph', async () => {
  const runtime = JSON.parse(await read('Frontend/package.json'));
  const samples = JSON.parse(await read('Samples/Frontend/package.json'));
  assert.deepEqual(Object.keys(runtime.dependencies), ['@vue/runtime-core']);
  for (const name of ['echarts', 'd3-hierarchy', '@tanstack/table-core', '@floating-ui/core', 'markdown-it']) {
    assert.ok(samples.dependencies[name], `${name} is declared by samples`);
    assert.equal(runtime.dependencies[name], undefined);
  }
  for (const name of ['@olton/animation', 'animejs', 'gsap']) {
    assert.ok(samples.devDependencies[name], `${name} is used by sample compatibility tests`);
    assert.equal(runtime.devDependencies[name], undefined);
  }
});
