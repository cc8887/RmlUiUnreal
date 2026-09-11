import { build } from 'esbuild';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import path from 'node:path';

const toolsRoot = path.resolve(import.meta.dirname, '..');
const outputRoot = path.resolve(toolsRoot, '..', 'Content', 'RuntimeCompiler');
await mkdir(outputRoot, { recursive: true });
await build({
  entryPoints: [path.join(toolsRoot, 'src', 'runtime-entry.mjs')],
  outfile: path.join(outputRoot, 'compiler.js'),
  bundle: true,
  platform: 'browser',
  format: 'cjs',
  target: 'es2020',
  external: ['puerts'],
  legalComments: 'none',
});

const packages = ['postcss', 'htmlparser2', 'domhandler', 'domutils', 'dom-serializer', 'domelementtype', 'entities', 'picocolors', 'nanoid', 'source-map-js'];
const notices = [];
for (const name of packages) {
  const packageRoot = path.join(toolsRoot, 'node_modules', ...name.split('/'));
  const metadata = JSON.parse(await readFile(path.join(packageRoot, 'package.json'), 'utf8'));
  const license = await readFile(path.join(packageRoot, 'LICENSE'), 'utf8');
  notices.push(`${metadata.name} ${metadata.version} (${metadata.license ?? 'see license text'})\n${license.trim()}`);
}
await writeFile(path.join(outputRoot, 'THIRD_PARTY_NOTICES.txt'), `${notices.join('\n\n-----\n\n')}\n`, 'utf8');
