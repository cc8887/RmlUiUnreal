import test from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { cp, mkdir, mkdtemp, readFile, writeFile, symlink } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const frontend = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
async function until(predicate, message) {
  const deadline = Date.now() + 10000;
  do { if (await predicate()) return; await pause(50); } while (Date.now() < deadline);
  assert.fail(message);
}
test('SFC watch publishes changes and retains the last version on compile failure', { timeout: 40000 }, async () => {
  const saved = path.resolve(frontend, '../../../Saved');
  await mkdir(saved, { recursive: true });
  const fixture = await mkdtemp(path.join(saved, 'VueBuildWatch-'));
  const copy = path.join(fixture, 'Plugins/RmlUiUnreal/Frontend');
  await mkdir(copy, { recursive: true });
  for (const item of ['src', 'tools', 'package.json']) await cp(path.join(frontend, item), path.join(copy, item), { recursive: true });
  await symlink(path.join(frontend, 'node_modules'), path.join(copy, 'node_modules'), 'junction');
  const assets = path.join(fixture, 'Plugins/RmlUiUnreal/Content/RmlUi');
  await mkdir(assets, { recursive: true });
  await cp(path.resolve(frontend, '../../RmlUiUnreal/Content/RmlUi/hello_world.png'), path.join(assets, 'hello_world.png'));
  const child = spawn(process.execPath, ['tools/build.mjs', '--watch'], { cwd: copy, windowsHide: true });
  let stdout = '', stderr = '';
  child.stdout.on('data', data => { stdout += data; });
  child.stderr.on('data', data => { stderr += data; });
  try {
    await until(() => stdout.includes('Watching Vue/TypeScript'), 'Watcher did not start: ' + stderr);
    const pointer = path.resolve(copy, '../Content/Vue/current.json');
    const initial = await readFile(pointer, 'utf8');
    const app = path.join(copy, 'src/App.vue');
    const source = await readFile(app, 'utf8');
    await writeFile(app, source + '\n<style>.watch-validation { color: #1177aa; }</style>\n');
    await until(async () => (await readFile(pointer, 'utf8')) !== initial, 'SFC save did not publish a version');
    const changed = await readFile(pointer, 'utf8');
    await writeFile(app, '<script setup>const = invalid;</script><template><div/></template>');
    await until(() => stderr.includes('ERROR'), 'Invalid SFC was not diagnosed');
    assert.equal(await readFile(pointer, 'utf8'), changed);
    await writeFile(app, source);
    await until(async () => (await readFile(pointer, 'utf8')) === initial, 'Watcher did not recover after correcting the SFC');
  } finally {
    const exited = once(child, 'exit');
    child.kill();
    await exited;
  }
});
