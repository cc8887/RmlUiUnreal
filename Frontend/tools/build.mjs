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
export async function buildFrontend({ entryPoint, outputRoot, mirrorRoot, activate = true,
  profile = 'dx11-compat', mode = 'strict', allowDegrade = [], requiredFeatures = [],
  tailwindContentFile, tailwindTheme, rewriteUnicodeProperties = false,
  requiredMotionRule = () => false, transformStyle, dependencyRoots = [], iconNames = [], iconDirectory,
  fontFiles = [], fontDirectory, assetFiles = [],
} = {}) {
  if (!entryPoint || !outputRoot) throw new Error('A frontend entry point and output directory are required.');
  if (iconNames.length && !iconDirectory) throw new Error('Icon names require an icon directory.');
  if (fontFiles.length && !fontDirectory) throw new Error('Font files require a font directory.');
  const output = path.resolve(outputRoot);
  const styles = new Map();
  const compilerOptions = { profile, mode, allowDegrade };
  const tailwindContent = tailwindContentFile ? await readFile(tailwindContentFile, 'utf8') : '';
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
          if (/@tailwind\b/.test(result.code)) {
            if (!tailwindContentFile) throw new Error(`${filename}: Tailwind directives require an explicit content source.`);
            cssRoot = (await postcss([tailwindcss({
              content: [{ raw: tailwindContent, extension: 'vue' }], corePlugins: { preflight: false },
              theme: tailwindTheme,
            })]).process(cssRoot, { from: filename })).root;
          }
          if (transformStyle) transformStyle(cssRoot, filename);
          componentStyles.push(validateCss(cssRoot, filename, { ...compilerOptions, sourceLabel, lineOffset: style.loc.start.line - 1 }));
        }
        styles.set(filename, componentStyles);
        return { contents: `${script.content}\n${templateCode}\n${descriptor.styles.some(s => s.scoped) ? `__component.__scopeId = '${id}';` : ''}\nexport default __component;`,
          loader: 'ts', resolveDir: path.dirname(filename) };
      });
    },
  };
  const result = await bundle({ entryPoints: [path.resolve(entryPoint)], bundle: true, write: false,
    platform: 'neutral', format: 'cjs', target: 'es2020', sourcemap: 'external', outfile: 'app.js',
    loader: { '.svg': 'text' },
    plugins: [vuePlugin], external: ['puerts'], nodePaths: dependencyRoots,
    conditions: ['module'], mainFields: ['module', 'main'], metafile: true,
    define: { 'process.env.NODE_ENV': '"production"', __VUE_OPTIONS_API__: 'true', __VUE_PROD_DEVTOOLS__: 'false', __VUE_PROD_HYDRATION_MISMATCH_DETAILS__: 'false' },
  });
  const files = new Map(result.outputFiles.map(file => [path.basename(file.path), file.contents]));
  const packages = new Set(Object.keys(result.metafile.inputs).flatMap(name => {
    const marker = name.lastIndexOf('node_modules/');
    if (marker < 0) return [];
    const parts = name.slice(marker + 'node_modules/'.length).split('/');
    return [parts[0].startsWith('@') ? parts.slice(0, 2).join('/') : parts[0]];
  }));
  if (iconNames.length) packages.add('lucide-static');
  for (const name of [...packages].sort()) {
    const directory = (await Promise.all(dependencyRoots.map(async directory => {
      const candidate = path.join(directory, name);
      return (await readdir(candidate).catch(() => null)) ? candidate : null;
    }))).find(Boolean);
    if (!directory) throw new Error(`Cannot find license source for bundled package ${name}.`);
    for (const file of (await readdir(directory)).filter(file => /^(license|copying)(\.|$)/i.test(file))) {
      const filename = `${name.replace(/[^a-zA-Z0-9_.-]/g, '_')}-${file}.txt`;
      files.set(`licenses/${filename}`, await readFile(path.join(directory, file)));
    }
  }
  if (rewriteUnicodeProperties) {
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
  const motionRules = compiledStyles.flatMap(value => value.motionManifest.rules).map(rule =>
    requiredMotionRule(rule) ? { ...rule, requiredOnLoad: true } : rule);
  const motionPlayStates = compiledStyles.flatMap(value => value.motionManifest.playStates ?? []);
  const motionManifest = motionPlayStates.length
    ? { schemaVersion: 1, rules: motionRules, playStates: motionPlayStates }
    : { schemaVersion: 1, rules: motionRules };
  const diagnostics = compiledStyles.flatMap(value => value.diagnostics);
  const capabilities = { ...mergeCapabilities(profile, compiledStyles.map(value => value.capabilities), requiredFeatures), diagnostics: 'compile-diagnostics.json' };
  files.set('compile-diagnostics.json', Buffer.from(JSON.stringify({ schemaVersion: 1, capabilities, diagnostics }, null, 2)));
  files.set('app.rcss', Buffer.from(css));
  files.set('motion-manifest.json', Buffer.from(`${JSON.stringify(motionManifest, null, 2)}\n`));
  files.set('shell.rml', Buffer.from('<rml><head><title>Vue RmlUi</title><link type="text/rcss" href="app.rcss"/></head><body/></rml>'));
  for (const { name, source } of assetFiles) files.set(name, await readFile(source));
  const fonts = [];
  for (const name of iconNames) {
    const svg = await readFile(path.join(iconDirectory, `${name}.svg`), 'utf8');
    for (const [suffix, color] of [['', '#586675'], ['-white', '#ffffff']]) {
      files.set(`icons/${name}${suffix}.png`, await sharp(Buffer.from(svg.replaceAll('currentColor', color))).resize(48, 48).png().toBuffer());
    }
  }
  for (const name of fontFiles) {
    files.set(`fonts/${name}`, await readFile(path.join(fontDirectory, name)));
    fonts.push(`fonts/${name}`);
  }
  const version = hash(JSON.stringify([...files].map(([name, bytes]) => [name, hash(bytes)]))).slice(0, 16);
  const digests = {};
  for (const [name, bytes] of files) digests[name] = hash(bytes);
  const manifest = JSON.stringify({ format: 1, abi: 1, stateSchema: 1, version, entry: 'app.js', document: 'shell.rml', motionManifest: 'motion-manifest.json', fonts, files: digests, capabilities }, null, 2);
  for (const destination of [...new Set([output, ...(mirrorRoot ? [path.resolve(mirrorRoot)] : [])])]) {
    const versionDirectory = path.join(destination, 'versions', version);
    await mkdir(versionDirectory, { recursive: true });
    for (const [name, bytes] of files) {
      await mkdir(path.dirname(path.join(versionDirectory, name)), { recursive: true });
      await writeFile(path.join(versionDirectory, name), bytes);
    }
    await writeFile(path.join(versionDirectory, 'manifest.json'), manifest);
    if (activate) {
      const temporary = path.join(destination, `current.${randomUUID()}.tmp`);
      await writeFile(temporary, JSON.stringify({ manifest: `versions/${version}/manifest.json` }, null, 2));
      await rename(temporary, path.join(destination, 'current.json'));
    }
  }
  const directory = path.join(output, 'versions', version);
  console.log(`Built Vue UI ${version}: ${directory}`);
  return { version, directory, capabilities, diagnostics };
}
export function watchFrontend({ watchRoots, build }) {
  if (!watchRoots?.length || typeof build !== 'function') throw new Error('Watch mode requires source directories and a build callback.');
    let timer, running = false, dirty = false;
    const rebuild = async () => {
      if (running) { dirty = true; return; }
      running = true;
      try { await build(); } catch (error) { console.error(error); }
      finally { running = false; if (dirty) { dirty = false; void rebuild(); } }
    };
    for (const source of watchRoots)
      watch(source, { recursive: true }, () => { clearTimeout(timer); timer = setTimeout(rebuild, 150); });
    console.log('Watching Vue/TypeScript sources.');
}
