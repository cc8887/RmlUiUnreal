import test from 'node:test';
import assert from 'node:assert/strict';
import { build } from 'esbuild';
import { mkdtemp, mkdir, readFile } from 'node:fs/promises';
import path from 'node:path';
import { pathToFileURL } from 'node:url';

const saved = path.resolve('../../../Saved/MarkdownTests');
await mkdir(saved, { recursive: true });
const folder = await mkdtemp(path.join(saved, 'adapter-'));
const output = path.join(folder, 'markdown.mjs');
await build({ entryPoints: ['src/chat/markdown.ts'], outfile: output, bundle: true, platform: 'neutral', mainFields: ['module', 'main'], format: 'esm' });
const { parseMarkdown, safeLink } = await import(pathToFileURL(output).href);
const all = tree => tree.flatMap(item => typeof item === 'string' ? [] : [item, ...all(item.children)]);
const plain = tree => tree.map(item => typeof item === 'string' ? item : plain(item.children)).join('');

test('mainstream markdown-it and highlight.js work without browser or Node built-ins', async () => {
  const bundle = await readFile(output, 'utf8');
  assert.ok(!bundle.includes('document.createElement'));
  const nodes = all(parseMarkdown('# Title\n\n**bold** *italic* ~~old~~ and `code`\n\n```cpp\nconst int columns = 3;\n```'));
  for (const tag of ['h1', 'strong', 'em', 's', 'code', 'pre']) assert.ok(nodes.some(node => node.tag === tag), tag);
  assert.ok(nodes.some(node => String(node.attrs.class).includes('hljs')));
});
test('tables and nested ordered lists become native Grid structures', () => {
  const nodes = all(parseMarkdown('| A | B |\n| :-- | --: |\n| one | two |\n\n3. third\n   - nested\n4. fourth'));
  const rows = nodes.filter(node => node.attrs.class === 'md-tr');
  assert.equal(rows.length, 2);
  assert.equal(rows[0].attrs.style.gridTemplateColumns, 'repeat(2, minmax(160px, 1fr))');
  assert.ok(nodes.some(node => node.attrs.class === 'md-td' && node.attrs.style?.textAlign === 'right'));
  assert.ok(nodes.some(node => node.attrs.class === 'md-marker' && node.children[0] === '3.'));
  assert.ok(nodes.some(node => node.attrs.class === 'md-marker' && node.children[0] === '4.'));
});
test('unclosed streaming fences remain code and preserve complete prefix blocks', () => {
  const partial = parseMarkdown('# Title\n\n```cpp\nconst int value =');
  const full = parseMarkdown('# Title\n\n```cpp\nconst int value = 3;\n```\n\nDone.');
  assert.deepEqual(partial[0], full[0]);
  assert.equal(partial[1].code, 'const int value =');
  assert.equal(full[1].code, 'const int value = 3;\n');
});
test('untrusted HTML and dangerous protocols never become executable nodes', () => {
  const source = '<script>alert(1)</script>\n\n[x](javascript:alert(1)) ![unsafe](file:///secret) [ok](https://example.com)';
  const tree = parseMarkdown(source), nodes = all(tree);
  assert.ok(plain(tree).includes('<script>alert(1)</script>'));
  assert.ok(!nodes.some(node => node.tag === 'script' || node.tag === 'img'));
  assert.deepEqual(nodes.filter(node => node.tag === 'a').map(node => node.attrs.href), ['https://example.com']);
  for (const url of ['javascript:alert(1)', 'data:text/html,x', 'file:///secret', 'https://host/\nscript']) assert.equal(safeLink(url), undefined);
});
test('Unicode, escapes, quotes, line breaks and local image assets survive the token adapter', () => {
  const tree = parseMarkdown('> \u4e2d\u6587 **\u6d41\u5f0f**\n\nline one  \nline two &amp; \\*plain\\*\n\n![sample](hello_world.png)');
  const nodes = all(tree);
  assert.ok(plain(tree).includes('\u4e2d\u6587 \u6d41\u5f0f'));
  assert.ok(plain(tree).includes('line two & *plain*'));
  assert.ok(nodes.some(node => node.tag === 'blockquote'));
  assert.ok(nodes.some(node => node.tag === 'br'));
  assert.ok(nodes.some(node => node.tag === 'img' && node.attrs.src === 'hello_world.png'));
});
