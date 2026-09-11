import { readFile, writeFile, mkdir } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import postcss from 'postcss';
const frontend = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const content = path.resolve(frontend, '../Content/Vue');
const pointer = JSON.parse(await readFile(path.join(content, 'current.json'), 'utf8'));
const manifestPath = path.join(content, pointer.manifest);
const original = JSON.parse(await readFile(manifestPath, 'utf8'));
const root = path.resolve(frontend, '../../../Saved/VueFixtures');
for (const kind of ['update2', 'bad-hash', 'bad-script', 'bad-abi']) {
  const directory = path.join(root, kind);
  await mkdir(directory, { recursive: true });
  const manifest = structuredClone(original);
  manifest.version = kind === 'update2' ? 'remote-version-2' : kind;
  if (kind === 'bad-abi') manifest.abi = 999;
  for (const filename of Object.keys(manifest.files)) {
    let bytes = await readFile(path.join(path.dirname(manifestPath), filename));
    if (kind === 'update2' && filename === 'app.rcss') {
      const css = postcss.parse(bytes.toString());
      css.append(postcss.rule({ selector: '#vue-header', nodes: [postcss.decl({ prop: 'border-bottom', value: '3px #4178b8' })] }));
      bytes = Buffer.from(css.toString());
    }
    if (kind === 'bad-script' && filename === 'app.js') bytes = Buffer.from('const = this is deliberately invalid JavaScript;');
    manifest.files[filename] = createHash('sha256').update(bytes).digest('hex');
    if (kind === 'bad-hash' && filename === 'app.js') bytes = Buffer.from('corrupted download');
    await mkdir(path.dirname(path.join(directory, filename)), { recursive: true });
    await writeFile(path.join(directory, filename), bytes);
  }
  await writeFile(path.join(directory, 'manifest.json'), JSON.stringify(manifest, null, 2));
}
console.log(root);
