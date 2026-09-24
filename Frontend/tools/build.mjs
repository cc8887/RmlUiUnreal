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
import { compileCss } from '../../Tools/src/compile-css.mjs';
import { mergeCapabilities } from '../../Tools/src/capabilities.mjs';
import { throwDiagnostics } from '../../Tools/src/compile-markup.mjs';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const hash = value => createHash('sha256').update(value).digest('hex');
export function validateCss(css, filename, options = {}) {
  const result = compileCss(css, { profile: 'slate-rhi', ...options, from: filename });
  throwDiagnostics(result.diagnostics);
  return result;
}
export async function buildFrontend({ chat = false, actors = false, outputRoot, activate = true,
  profile = actors ? 'slate-rhi' : 'dx11-compat', mode = actors ? 'degrade' : 'strict',
  // This demo displays existing shadow specimens on Slate. Their unsupported layers are
  // intentionally removed and recorded in the versioned diagnostics artifact.
  allowDegrade = actors ? ['render.layers', 'render.filters'] : [],
  requiredFeatures = actors ? ['nodes.query', 'layout.measure', 'events.extended', 'overlays.modal', 'input.ime'] : [],
} = {}) {
  if (chat && actors) throw new Error('Chat and actor observer builds are separate entry points.');
  const output = outputRoot ? path.resolve(outputRoot) : path.resolve(root, actors ? '../Content/ActorObserver' : chat ? '../Content/Chat' : '../Content/Vue');
  const styles = new Map();
  const compilerOptions = { profile, mode, allowDegrade };
  const tailwindContent = actors ? await readFile(path.join(root, 'src/actors/ActorObserverApp.vue'), 'utf8') : '';
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
          if (/v-html|\.passive\b/.test(descriptor.template.content)) throw new Error('v-html and passive modifiers are outside the RmlUi event contract.');
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
          const sourceLabel = path.relative(root, filename).split(path.sep).join('/');
          let cssRoot = postcss.parse(result.code, { from: filename });
          if (actors && /@tailwind\b/.test(result.code)) {
            cssRoot = (await postcss([tailwindcss({
              content: [{ raw: tailwindContent, extension: 'vue' }], corePlugins: { preflight: false },
              theme: { extend: { colors: { ink: '#202a2e', signal: '#14866d', warning: '#c27a28' } } },
            })]).process(cssRoot, { from: filename })).root;
          }
          componentStyles.push(validateCss(cssRoot, filename, { ...compilerOptions, sourceLabel, lineOffset: style.loc.start.line - 1 }));
        }
        styles.set(filename, componentStyles);
        return { contents: `${script.content}\n${templateCode}\n${descriptor.styles.some(s => s.scoped) ? `__component.__scopeId = '${id}';` : ''}\nexport default __component;`,
          loader: 'ts', resolveDir: path.dirname(filename) };
      });
    },
  };
  const entry = actors ? 'src/actors/main.ts' : chat ? 'src/chat/main.ts' : 'src/main.ts';
  const result = await bundle({ entryPoints: [path.join(root, entry)], bundle: true, write: false,
    platform: 'neutral', format: 'cjs', target: 'es2020', sourcemap: 'external', outfile: 'app.js',
    loader: { '.svg': 'text' },
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
  const compiledStyles = [...styles].sort(([a], [b]) => a.localeCompare(b)).flatMap(([, value]) => value);
  const css = compiledStyles.map(value => value.css).join('\n');
  const diagnostics = compiledStyles.flatMap(value => value.diagnostics);
  const capabilities = { ...mergeCapabilities(profile, compiledStyles.map(value => value.capabilities), requiredFeatures), diagnostics: 'compile-diagnostics.json' };
  files.set('compile-diagnostics.json', Buffer.from(JSON.stringify({ schemaVersion: 1, capabilities, diagnostics }, null, 2)));
  files.set('app.rcss', Buffer.from(css));
  files.set('shell.rml', Buffer.from('<rml><head><title>Vue RmlUi</title><link type="text/rcss" href="app.rcss"/></head><body/></rml>'));
  const content = path.resolve(root, '../Content/RmlUi');
  files.set('hello_world.png', await readFile(path.join(content, 'hello_world.png')));
  const fonts = [];
  if (chat || actors) {
    const icons = actors ? ['panel-bottom', 'pause', 'play', 'list-tree', 'panels-top-left', 'git-branch', 'zoom-in', 'zoom-out', 'maximize-2', 'search', 'circle-plus', 'eye', 'settings-2', 'save', 'trash-2', 'app-window', 'triangle-alert', 'panel-right-open', 'x', 'sparkles', 'rotate-ccw', 'chevron-down'] : ['arrow-up', 'arrow-down', 'square', 'copy', 'check', 'plus', 'pencil', 'rotate-ccw', 'settings-2', 'panel-left', 'x', 'trash-2', 'messages-square'];
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
  await writeFile(path.join(directory, 'manifest.json'), JSON.stringify({ format: 1, abi: 1, stateSchema: 1, version, entry: 'app.js', document: 'shell.rml', fonts, files: digests, capabilities }, null, 2));
  if (activate) {
    const temporary = path.join(output, `current.${randomUUID()}.tmp`);
    await writeFile(temporary, JSON.stringify({ manifest: `versions/${version}/manifest.json` }, null, 2));
    await rename(temporary, path.join(output, 'current.json'));
  }
  console.log(`Built Vue UI ${version}: ${directory}`);
  return { version, directory, capabilities, diagnostics };
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
