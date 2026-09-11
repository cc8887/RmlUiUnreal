import { build as bundle } from 'esbuild';
import { parse, compileScript, compileTemplate, compileStyle } from '@vue/compiler-sfc';
import postcss from 'postcss';
import tailwindcss from 'tailwindcss';
import { createHash, randomUUID } from 'node:crypto';
import { readFile, writeFile, mkdir, rename, readdir } from 'node:fs/promises';
import { watch } from 'node:fs';
import path from 'node:path';
import sharp from 'sharp';
import { transformSync } from '@babel/core';
import unicodeProperties from '@babel/plugin-transform-unicode-property-regex';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const hash = value => createHash('sha256').update(value).digest('hex');
const allowed = new Set(('display position top left right bottom width height min-width min-height max-width max-height box-sizing overflow overflow-x overflow-y ' +
  'margin margin-top margin-right margin-bottom margin-left padding padding-top padding-right padding-bottom padding-left ' +
  'color background background-color font-family font-size font-weight font-style line-height text-align text-decoration white-space word-break vertical-align ' +
  'border border-width border-color border-style border-radius border-top border-bottom border-left border-right border-left-width border-right-width ' +
  'border-top-width border-bottom-width border-left-color opacity cursor visibility z-index ' +
  'flex flex-grow flex-shrink flex-basis flex-direction flex-wrap align-items align-self align-content justify-content justify-items justify-self order ' +
  'gap row-gap column-gap grid-template-columns grid-template-rows grid-template-areas grid-area grid-row grid-column grid-auto-flow grid-auto-rows grid-auto-columns ' +
  'decorator filter box-shadow transform transform-origin animation animation-delay animation-duration animation-iteration-count').split(/\s+/));
export function validateCss(css, filename) {
  postcss.parse(css, { from: filename }).walkDecls(declaration => {
    if (!allowed.has(declaration.prop)) throw declaration.error(`Unsupported RmlUi CSS property: ${declaration.prop}`);
    if (/\b(var|env)\(/.test(declaration.value)) throw declaration.error('CSS variables and env() are not implemented by this adapter.');
  });
}
function normalizeTailwindForRmlUi(css) {
  const root = postcss.parse(css, { from: 'tailwind.css' });
  root.walkDecls(declaration => {
    if (declaration.prop.startsWith('--tw-')) { declaration.remove(); return; }
    declaration.value = declaration.value.replace(
      /rgb\((\d+)\s+(\d+)\s+(\d+)\s*\/\s*var\(--[^,()]+(?:,\s*[\d.]+)?\)\)/g,
      'rgb($1,$2,$3)',
    );
    if (/\bvar\(/.test(declaration.value)) declaration.remove();
    declaration.value = declaration.value.replace(/minmax\(0\s*,/g, 'minmax(0px,');
  });
  root.walkRules(rule => { if (!rule.nodes?.length) rule.remove(); });
  return root.toString();
}
export async function buildFrontend({ chat = false, actors = false } = {}) {
  if (chat && actors) throw new Error('Chat and actor observer builds are separate entry points.');
  const output = path.resolve(root, actors ? '../Content/ActorObserver' : chat ? '../Content/Chat' : '../Content/Vue');
  const styles = new Map();
  const vuePlugin = {
    name: 'rmlui-vue-sfc',
    setup(builder) {
      builder.onResolve({ filter: /^(vue|@rmlui\/vue)$/ }, () => ({ path: path.join(root, 'src/renderer.ts') }));
      builder.onLoad({ filter: /\.vue$/ }, async ({ path: filename }) => {
        const source = await readFile(filename, 'utf8');
        const { descriptor, errors } = parse(source, { filename });
        if (errors.length) throw new Error(errors.join('\n'));
        if (descriptor.styles.some(style => style.src || style.module)) throw new Error('External SFC styles and CSS modules require explicit adapter support.');
        const id = `data-v-${hash(path.relative(root, filename).split(path.sep).join('/')).slice(0, 8)}`;
        const componentStyles = [];
        const script = compileScript(descriptor, { id, genDefaultAs: '__component' });
        let templateCode = '';
        if (descriptor.template) {
          if (/v-html|\.(prevent|passive|exact)\b/.test(descriptor.template.content)) throw new Error('v-html and prevent/passive/exact modifiers are outside the RmlUi event contract.');
          const template = compileTemplate({ source: descriptor.template.content, filename, id,
            scoped: descriptor.styles.some(style => style.scoped),
            compilerOptions: { runtimeModuleName: '@rmlui/vue', bindingMetadata: script.bindings, hoistStatic: false },
          });
          if (template.errors.length) throw new Error(template.errors.join('\n'));
          templateCode = template.code + '\n__component.render = render;';
        }
        for (const style of descriptor.styles) {
          const result = compileStyle({ source: style.content, filename, id, scoped: !!style.scoped });
          if (result.errors.length) throw new Error(result.errors.join('\n'));
          validateCss(result.code, filename); componentStyles.push(result.code);
        }
        styles.set(filename, componentStyles.join('\n'));
        return { contents: `${script.content}\n${templateCode}\n${descriptor.styles.some(s => s.scoped) ? `__component.__scopeId = '${id}';` : ''}\nexport default __component;`,
          loader: 'ts', resolveDir: path.dirname(filename) };
      });
    },
  };
  const entry = actors ? 'src/actors/main.ts' : chat ? 'src/chat/main.ts' : 'src/main.ts';
  const result = await bundle({ entryPoints: [path.join(root, entry)], bundle: true, write: false,
    platform: 'neutral', format: 'cjs', target: 'es2020', sourcemap: 'external', outfile: 'app.js',
    plugins: [vuePlugin], external: ['puerts'], conditions: ['module'], mainFields: ['module', 'main'], metafile: true,
    define: { 'process.env.NODE_ENV': '"production"', __VUE_OPTIONS_API__: 'true', __VUE_PROD_DEVTOOLS__: 'false', __VUE_PROD_HYDRATION_MISMATCH_DETAILS__: 'false' },
  });
  const files = new Map(result.outputFiles.map(file => [path.basename(file.path), file.contents]));
  const packages = new Set(Object.keys(result.metafile.inputs).filter(name => name.startsWith('node_modules/')).map(name => {
    const parts = name.slice('node_modules/'.length).split('/'); return parts[0].startsWith('@') ? parts.slice(0, 2).join('/') : parts[0];
  }));
  if (chat || actors) packages.add('lucide-static');
  for (const name of [...packages].sort()) {
    const directory = path.join(root, 'node_modules', name);
    for (const file of (await readdir(directory)).filter(file => /^(license|copying)(\.|$)/i.test(file))) {
      const filename = `${name.replace(/[^a-zA-Z0-9_.-]/g, '_')}-${file}.txt`;
      files.set(`licenses/${filename}`, await readFile(path.join(directory, file)));
    }
  }
  if (chat) {
    const transformed = transformSync(Buffer.from(files.get('app.js')).toString(), {
      filename: 'app.js', configFile: false, babelrc: false, sourceMaps: true,
      inputSourceMap: JSON.parse(Buffer.from(files.get('app.js.map')).toString()),
      plugins: [[unicodeProperties, { useUnicodeFlag: false }]],
    });
    files.set('app.js', Buffer.from(transformed.code));
    files.set('app.js.map', Buffer.from(JSON.stringify(transformed.map)));
  }
  let css = [...styles].sort(([a], [b]) => a.localeCompare(b)).map(([, value]) => value).join('\n');
  if (actors) {
    const content = await readFile(path.join(root, 'src/actors/ActorObserverApp.vue'), 'utf8');
    css = (await postcss([tailwindcss({
      content: [{ raw: content, extension: 'vue' }],
      corePlugins: { preflight: false },
      theme: { extend: { colors: { ink: '#202a2e', signal: '#14866d', warning: '#c27a28' } } },
    })]).process(css, { from: 'ActorObserverApp.vue' })).css;
    css = normalizeTailwindForRmlUi(css);
    validateCss(css, 'ActorObserverApp.vue');
  }
  files.set('app.rcss', Buffer.from(css));
  files.set('shell.rml', Buffer.from('<rml><head><title>Vue RmlUi</title><link type="text/rcss" href="app.rcss"/></head><body/></rml>'));
  const content = path.resolve(root, '../Content/RmlUi');
  files.set('hello_world.png', await readFile(path.join(content, 'hello_world.png')));
  const fonts = [];
  if (chat || actors) {
    const icons = actors ? ['panel-bottom', 'pause', 'play', 'list-tree', 'panels-top-left', 'search', 'circle-plus', 'eye', 'settings-2', 'save', 'trash-2'] : ['arrow-up', 'arrow-down', 'square', 'copy', 'check', 'plus', 'pencil', 'rotate-ccw', 'settings-2', 'panel-left', 'x', 'trash-2', 'messages-square'];
    for (const name of icons) {
      const svg = await readFile(path.join(root, 'node_modules/lucide-static/icons', `${name}.svg`), 'utf8');
      for (const [suffix, color] of [['', '#586675'], ['-white', '#ffffff']]) {
        files.set(`icons/${name}${suffix}.png`, await sharp(Buffer.from(svg.replaceAll('currentColor', color))).resize(48, 48).png().toBuffer());
      }
    }
    const fontNames = actors ? ['NotoSansCJKsc-Regular.otf', 'JetBrainsMono.ttf'] : ['NotoSansCJKsc-Regular.otf', 'JetBrainsMono.ttf', 'LatoLatin-Italic.ttf', 'LatoLatin-BoldItalic.ttf'];
    for (const name of fontNames) {
      files.set(`fonts/${name}`, await readFile(path.join(root, 'assets', name))); fonts.push(`fonts/${name}`);
    }
    for (const name of ['OFL-Noto.txt', 'OFL-JetBrains.txt']) files.set(`fonts/${name}`, await readFile(path.join(root, 'assets', name)));
    files.set('fonts/OFL-Lato.txt', await readFile(path.join(content, 'Fonts/LICENSE.txt')));
  }
  const version = hash(JSON.stringify([...files].map(([name, bytes]) => [name, hash(bytes)]))).slice(0, 16);
  const directory = path.join(output, 'versions', version);
  await mkdir(directory, { recursive: true });
  const digests = {};
  for (const [name, bytes] of files) { await mkdir(path.dirname(path.join(directory, name)), { recursive: true }); await writeFile(path.join(directory, name), bytes); digests[name] = hash(bytes); }
  await writeFile(path.join(directory, 'manifest.json'), JSON.stringify({ format: 1, abi: 1, stateSchema: 1, version, entry: 'app.js', document: 'shell.rml', fonts, files: digests }, null, 2));
  const temporary = path.join(output, `current.${randomUUID()}.tmp`);
  await writeFile(temporary, JSON.stringify({ manifest: `versions/${version}/manifest.json` }, null, 2));
  await rename(temporary, path.join(output, 'current.json'));
  console.log(`Built Vue UI ${version}: ${directory}`);
  return { version, directory };
}
if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const options = { chat: process.argv.includes('--chat'), actors: process.argv.includes('--actors') };
  await buildFrontend(options);
  if (process.argv.includes('--watch')) {
    let timer, running = false, dirty = false;
    const rebuild = async () => {
      if (running) { dirty = true; return; }
      running = true;
      try { await buildFrontend(options); } catch (error) { console.error(error); }
      finally { running = false; if (dirty) { dirty = false; void rebuild(); } }
    };
    watch(path.join(root, 'src'), { recursive: true }, () => { clearTimeout(timer); timer = setTimeout(rebuild, 150); });
    console.log('Watching Vue/TypeScript sources.');
  }
}
