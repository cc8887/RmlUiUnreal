import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, readFile, readdir } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { createHash } from 'node:crypto';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { build as bundle } from 'esbuild';
import { validateCss } from '../../../Frontend/tools/build.mjs';
import { buildSample } from '../tools/build.mjs';
import { scopeDemoStyles } from '../tools/build.mjs';
import postcss from 'postcss';

test('embedded sample styles stay inside their tab while keyframe selectors retain their meaning', () => {
  const css = postcss.parse('body, button { color:red } @media (max-width:700px) { .card { width:100% } } @keyframes fade { from { opacity:0 } to { opacity:1 } }');
  scopeDemoStyles(css, path.join(process.cwd(), 'src', 'chat', 'ChatApp.vue'));
  assert.match(css.toString(), /#chat-tab,\s*#chat-tab button/);
  assert.match(css.toString(), /#chat-tab \.card/);
  assert.match(css.toString(), /@keyframes fade \{ from \{/);
  assert.doesNotMatch(css.toString(), /#chat-tab from/);
});

test('published sample versions retain every content-addressed asset', async () => {
  const content = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../RmlUiUnrealSamples/Content');
  for (const app of ['ActorObserver', 'Chat', 'Vue']) {
    const appDirectory = path.join(content, app);
    const pointer = JSON.parse(await readFile(path.join(appDirectory, 'current.json'), 'utf8'));
    const versionNames = await readdir(path.join(appDirectory, 'versions'));
    assert.ok(versionNames.length > 0, `${app} has published versions`);
    for (const version of versionNames) {
      const versionDirectory = path.join(appDirectory, 'versions', version);
      const manifest = JSON.parse(await readFile(path.join(versionDirectory, 'manifest.json'), 'utf8'));
      assert.equal(manifest.version, version);
      for (const [name, expected] of Object.entries(manifest.files)) {
        const data = await readFile(path.join(versionDirectory, name));
        assert.equal(createHash('sha256').update(data).digest('hex'), expected, `${app}/${version}/${name}`);
      }
    }
    assert.ok(versionNames.includes(path.basename(path.dirname(pointer.manifest))), `${app} pointer resolves`);
  }
});

test('CSS diagnostics retain the implemented grid syntax', () => {
  assert.doesNotThrow(() => validateCss('.grid { display:grid; grid-template-columns:repeat(3,minmax(0px,1fr)); grid-template-areas:"a b c"; gap:16px; }', 'grid.css'));
  assert.throws(() => validateCss('.x { backdrop-filter:blur(3px); }', 'bad.css'), /unsupported-renderer-feature/);
  assert.doesNotThrow(() => validateCss('.x { --color:#f6c543; color:var(--color); }', 'theme.css'));
  assert.throws(() => validateCss('.x { position:sticky; }', 'bad.css'), /unsupported-css-value/);
});
test('actual SFC bundle has reproducible content-addressed versions and complete hashes', async () => {
  const outputRoot = await mkdtemp(path.join(tmpdir(), 'rmlui-vue-build-'));
  const first = await buildSample({ outputRoot, activate: false }), second = await buildSample({ outputRoot, activate: false });
  assert.equal(first.version, second.version);
  const manifest = JSON.parse(await readFile(path.join(first.directory, 'manifest.json'), 'utf8'));
  const motionManifest = JSON.parse(await readFile(path.join(first.directory, manifest.motionManifest), 'utf8'));
  assert.equal(manifest.capabilities.profile, 'dx11-compat');
  assert.equal(manifest.capabilities.minimumHostAbi, 2);
  assert.ok(manifest.capabilities.requiredFeatures.includes('css.grid'));
  assert.equal(manifest.motionManifest, 'motion-manifest.json');
  assert.equal(motionManifest.schemaVersion, 1);
  await assert.rejects(readFile(path.join(outputRoot, 'current.json')), { code: 'ENOENT' });
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
  const outputRoot = await mkdtemp(path.join(tmpdir(), 'rmlui-actor-build-'));
  const result = await buildSample({ app: 'actors', outputRoot, activate: false });
  const css = await readFile(path.join(result.directory, 'app.rcss'), 'utf8');
  const source = await readFile(path.join(result.directory, 'app.js'), 'utf8');
  const manifest = JSON.parse(await readFile(path.join(result.directory, 'manifest.json'), 'utf8'));
  const motionManifest = JSON.parse(await readFile(path.join(result.directory, manifest.motionManifest), 'utf8'));
  assert.match(css, /\.grid-cols-3/);
  assert.match(css, /minmax\(0px,\s*1fr\)/);
  assert.ok(css.includes('var(--tw-'), 'Tailwind runtime tokens are retained');
  assert.ok(css.includes('--tw-bg-opacity:'), 'Tailwind opacity declarations are not silently removed');
  assert.match(css, /grid-template-areas:\s*"icons image"/);
  assert.match(css, /\.text-scroll\s*\{[^}]*overflow:\s*auto/);
  assert.match(css, /\.split-handle\s*\{[^}]*drag:\s*drag/);
  assert.doesNotMatch(css, /box-shadow:/, 'the Slate demo explicitly degrades unsupported layer effects');
  assert.ok(result.capabilities.degradedFeatures.includes('render.layers'));
  assert.ok(result.diagnostics.some(item => item.classification === 'degraded' && item.property === 'box-shadow' && item.source.endsWith('.vue') && item.line > 0));
  assert.match(css, /@keyframes scan-line/);
  assert.ok(source.includes('GetActorSnapshot'));
  assert.ok(source.includes('chat-view-tab'));
  assert.ok(source.includes('vue-dashboard-view-tab'));
  assert.match(css, /#chat-tab #chat-app/);
  assert.match(css, /#vue-tab #vue-workspace/);
  assert.match(css, /\.swatch\.teal\s*\{/);
  assert.doesNotMatch(css, /(^|\})\s*\.teal\s*\{/);
  assert.doesNotMatch(css, /(^|\})\s*#chat-app\s*\{/);
  assert.doesNotMatch(css, /(^|\})\s*#vue-workspace\s*\{/);
  assert.ok(source.includes('GetActorDetails'));
  assert.ok(source.includes('hello_world.png'));
  assert.ok(source.includes('Pause live refresh'));
  assert.ok(source.includes('ui-lab-scroll'));
  assert.ok(source.includes('ui-lab-resizer'));
  assert.ok(source.includes('onDragstart'));
  assert.ok(source.includes('onDrag'));
  assert.ok(source.includes('onDragend'));
  assert.ok(source.includes('renderToSVGString'));
  assert.ok(source.includes('SetInnerRml'));
  assert.ok(source.includes('chart-slider-'));
  assert.ok(source.includes('tree-view-tab'));
  assert.ok(source.includes('motion-view-tab'));
  assert.ok(source.includes('motion-menu-showcase'));
  assert.ok(source.includes('motion-option-'));
  assert.ok(source.includes('animation-view-tab'));
  assert.ok(source.includes('animation-spring-showcase'));
  assert.ok(source.includes('animation-spring-replay'));
  assert.ok(source.includes('animation-official-view-tab'));
  assert.ok(source.includes('animation-official-examples'));
  assert.ok(source.includes('css-motion-view-tab'));
  assert.ok(source.includes('native-css-animation-showcase'));
  assert.ok(source.includes('css-motion-replay'));
  assert.ok(source.includes('css-control-pause'));
  assert.ok(source.includes('css-control-duplicate'));
  assert.ok(source.includes('css-control-rebind'));
  assert.ok(source.includes('RestartCssAnimation'));
  assert.equal(motionManifest.rules.length, 5);
  assert.ok(motionManifest.rules.every(rule => rule.requiredOnLoad === true));
  assert.equal(motionManifest.rules.reduce((count, rule) => count + rule.tracks.length, 0), 10);
  assert.deepEqual(motionManifest.rules.slice(0, 4).map(rule => rule.delay), [0.2, 0.55, 0.9, 1.25]);
  assert.ok(motionManifest.rules.slice(0, 4).every(rule => rule.duration === 2.8));
  assert.ok(motionManifest.rules.slice(0, 4).every(rule => rule.selector.startsWith('.css-motion-run.css-motion-item-')));
  assert.deepEqual({ selector: motionManifest.rules[4].selector, delay: motionManifest.rules[4].delay,
    iterations: motionManifest.rules[4].iterations, direction: motionManifest.rules[4].direction },
  { selector: '.css-control-loop', delay: -0.55, iterations: 0, direction: 2 });
  assert.deepEqual(motionManifest.playStates.slice(-2), [
    { selector: '.css-control-loop', paused: false },
    { selector: '.css-control-paused', paused: true },
  ]);
  assert.doesNotMatch(css, /@keyframes css-motion-rise/);
  assert.doesNotMatch(css, /animation-name:\s*css-motion-rise/);
  assert.ok(source.includes('CSS compiler'));
  assert.ok(source.includes('animation adapter'));
  assert.ok(source.includes('Compatibility routes'));
  assert.ok(source.includes('Slate-RHI limited'));
  assert.ok(source.includes('Effects.swirlIn'));
  assert.ok(source.includes('talent-ring-'));
  assert.ok(source.includes('easeInOutQuad'));
  assert.ok(source.includes('translateY'));
  assert.ok(source.includes('1.28'));
  assert.ok(source.includes('cubic-bezier(0.16,1.32,0.3,1)'));
  assert.ok(source.includes('stagger'));
  assert.match(css, /\.t-dropdown\s*\{[^}]*transform-origin:/);
  assert.match(css, /\.t-dropdown\.is-closing\s*\{[^}]*transition:transform 0\.15s cubic-out/);
  assert.ok(source.includes('tree-visible-count'));
  assert.ok(source.includes('Duplicate tree node id'));
  assert.ok(source.includes('dialogs-view-tab'));
  assert.ok(source.includes('headless-dialog-trigger'));
  assert.ok(source.includes('shadcn-alert-dialog'));
  assert.ok(source.includes('shadcn-sheet'));
  assert.ok(source.includes('FocusNode'));
  assert.ok(source.includes('SetUiMaterialIntensity'));
  assert.ok(source.includes('ui-material-slider'));
  assert.ok(source.includes('mask-view-tab'));
  assert.ok(source.includes('edge-mask'));
  assert.ok(source.includes('mask-probe-count'));
  assert.ok(source.includes('mask-motion'));
  assert.ok(source.includes('gold-flow-'));
  assert.ok(source.includes('gold-spark'));
  assert.ok(source.includes('scene-view-tab'));
  assert.ok(source.includes('scene-overlay-showcase'));
  assert.ok(source.includes('scene-opacity'));
  assert.match(css, /body\s*\{[^}]*background-color:\s*transparent/);
  assert.match(css, /\.scene-panel\s*\{[^}]*pointer-events:\s*auto/);
  assert.match(css, /\.scene-open-window\s*\{[^}]*pointer-events:\s*none/);
  assert.match(css, /\.tree-stage\s*\{[^}]*drag:\s*drag/);
  assert.match(css, /\.material-preview\s*\{[^}]*decorator:\s*ue-material\(showcase\.energy\)/);
  assert.match(css, /\.gold-edge-mask[^\{]*\{[^}]*position:\s*fixed[^}]*pointer-events:\s*none/);
  assert.match(css, /@keyframes gold-spark-down\s*\{/);
  assert.ok(source.includes('AnimateNode'), 'flow animation is started after native layout');
  for (const edge of ['top', 'right', 'bottom', 'left']) assert.match(source, new RegExp(`side:\\s*["']${edge}["']`));
  const maskSvg = await readFile(path.resolve('assets/gold-edge-mask.svg'), 'utf8');
  assert.match(maskSvg, /<mask\b[^>]*id="edge-mask"/);
  assert.match(maskSvg, /mask="url\(#edge-mask\)"/);
  assert.match(maskSvg, /id="glow-top"/);
  assert.match(maskSvg, /id="ray-top"/);
  assert.match(css, /\.rml-dialog-overlay\s*\{[^}]*position:\s*absolute/);
  assert.match(css, /\.rml-dialog-backdrop\s*\{[^}]*position:\s*absolute/);
});

test('d3-hierarchy produces native tree node positions, edges and collapsed layouts', async () => {
  const result = await bundle({
    entryPoints: [path.resolve('src/tree/d3TreeLayout.ts')],
    bundle: true,
    write: false,
    platform: 'node',
    format: 'esm',
  });
  const module = await import(`data:text/javascript;base64,${Buffer.from(result.outputFiles[0].text).toString('base64')}`);
  const tree = {
    id: 'root', label: 'Root', detail: 'root', kind: 'runtime', children: [
      { id: 'branch', label: 'Branch', detail: 'branch', kind: 'bridge', children: [
        { id: 'leaf', label: 'Leaf', detail: 'leaf', kind: 'tooling' },
      ] },
      { id: 'peer', label: 'Peer', detail: 'peer', kind: 'render' },
    ],
  };
  const expanded = module.layoutTree(tree, new Set());
  assert.equal(expanded.nodes.length, 4);
  assert.equal(expanded.edges.length, 9);
  assert.ok(expanded.nodes.every(node => Number.isFinite(node.left) && Number.isFinite(node.top)));
  assert.ok(expanded.width > module.TREE_NODE_WIDTH && expanded.height > module.TREE_NODE_HEIGHT);
  const collapsed = module.layoutTree(tree, new Set(['branch']));
  assert.deepEqual(collapsed.nodes.map(node => node.id), ['root', 'branch', 'peer']);
  assert.equal(collapsed.edges.length, 6);
  assert.equal(collapsed.nodes.find(node => node.id === 'branch').collapsed, true);
});

test('ECharts produces chart SVG without a DOM or Canvas', async () => {
  const result = await bundle({
    entryPoints: [path.resolve('src/charts/echartsSvg.ts')],
    bundle: true,
    write: false,
    platform: 'node',
    format: 'esm',
  });
  const source = result.outputFiles[0].text;
  const module = await import(`data:text/javascript;base64,${Buffer.from(source).toString('base64')}`);
  const svg = module.renderEditableChartSvg([
    { label: 'Render', value: 68, target: 82 },
    { label: 'Layout', value: 54, target: 72 },
  ], 480, 240);
  assert.match(svg, /^<svg\b/);
  assert.match(svg, /<path\b/);
  assert.match(svg, />Render<|>Layout</);
  assert.ok(!svg.includes('<canvas'));
});
