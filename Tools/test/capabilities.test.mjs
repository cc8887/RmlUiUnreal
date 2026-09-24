import assert from 'node:assert/strict';
import test from 'node:test';
import { build } from 'esbuild';
import vm from 'node:vm';
import { readFile } from 'node:fs/promises';
import { compileCss } from '../src/compile-css.mjs';
import { compileDocumentMarkup } from '../src/compile-markup.mjs';
import { capabilityCatalog, createCapabilities, mergeCapabilities } from '../src/capabilities.mjs';
import { capabilityHeaderPath, generateCapabilityHeader } from '../src/generate-capability-catalog.mjs';

const strict = { profile: 'slate-rhi', from: 'components/Panel.vue' };
const errors = result => result.diagnostics.filter(item => item.severity === 'error');

test('the runtime C++ catalog and manifest schema match the single capability catalog', async () => {
  assert.equal(await readFile(capabilityHeaderPath, 'utf8'), generateCapabilityHeader(), 'Regenerate the C++ catalog after changing capabilities.json');
  const schema = JSON.parse(await readFile(new URL('../src/capabilities.schema.json', import.meta.url), 'utf8'));
  assert.deepEqual([...schema.$defs.feature.enum].sort(), Object.keys(capabilityCatalog.features).sort());
  assert.equal(schema.properties.compiler.const, capabilityCatalog.compiler);
  assert.equal(schema.properties.minimumHostAbi.minimum, capabilityCatalog.minimumHostAbi);
  assert.deepEqual([...schema.properties.profile.enum].sort(), Object.keys(capabilityCatalog.profiles).filter(name => !capabilityCatalog.profiles[name].deprecated).sort());
});

test('renderer profiles gate final effects and retain actionable source locations', () => {
  const source = '.panel {\n  box-shadow:0 4px 8px #0008;\n  filter:blur(4px);\n  decorator:linear-gradient(#000,#fff);\n}';
  const rejected = compileCss(source, strict);
  assert.equal(errors(rejected).length, 3);
  assert.equal(errors(rejected)[0].line, 2);
  assert.equal(errors(rejected)[0].selector, '.panel');
  assert.equal(errors(rejected)[0].classification, 'rejected');
  const dx11 = compileCss(source, { ...strict, profile: 'dx11-compat' });
  assert.deepEqual(errors(dx11), []);
  assert.ok(dx11.capabilities.requiredFeatures.includes('render.filters'));
  const degraded = compileCss(source, { ...strict, mode: 'degrade', allowDegrade: ['render.layers', 'render.filters', 'render.shaders'] });
  assert.deepEqual(errors(degraded), []);
  assert.doesNotMatch(degraded.css, /box-shadow:|filter:|decorator:/);
  assert.equal(degraded.diagnostics.filter(item => item.classification === 'degraded').length, 3);
  assert.deepEqual(degraded.capabilities.requiredFeatures, ['css.core']);
});

test('profile and required-feature identities are closed and versioned', () => {
  assert.throws(() => compileCss('', { profile: 'future-browser' }), /Unknown.*profile/);
  assert.throws(() => createCapabilities('slate-rhi', ['unknown.feature']), /Unknown.*capability/);
  assert.throws(() => createCapabilities('slate-rhi', ['render.layers']), /does not provide/);
  const result = mergeCapabilities('slate-rhi', [createCapabilities('slate-rhi', ['css.variables'])], ['nodes.query']);
  assert.equal(result.minimumHostAbi, 2);
  assert.equal(result.minimumSlateAbi, 5);
  assert.deepEqual(result.requiredFeatures, ['css.variables', 'nodes.query']);
});

test('theme and Tailwind variables stay live with native-correct alpha', () => {
  const result = compileCss(`:root { --brand:#f0d283; --tw-bg-opacity:0.6; --tw-translate-x:12px; }
    .dark { --brand:#90702b; }
    .card { color:var(--brand,#fff); background-color:rgb(20 100 160 / var(--tw-bg-opacity)); transform:translate3d(var(--tw-translate-x),0px,0px); }
    .dim { color:rgba(20,100,160,0.25); }`, strict);
  assert.deepEqual(errors(result), []);
  assert.match(result.css, /--tw-bg-opacity:0\.6/);
  assert.match(result.css, /color:var\(--brand,#fff\)/);
  assert.match(result.css, /hsla\([^;]+var\(--tw-bg-opacity\)\)/);
  assert.match(result.css, /rgba\(20,100,160,25%\)/);
  assert.match(result.css, /transform:translate\(var\(--tw-translate-x\),0px\)/);
  assert.ok(result.capabilities.requiredFeatures.includes('css.variables'));
  assert.ok(!result.capabilities.requiredFeatures.includes('render.transform3d'));
});

test('declaration values and opaque modern syntax do not pass a property-only whitelist', () => {
  for (const declaration of ['position:sticky', 'color:color-mix(in srgb,red,blue)', 'width:calc(100% - 2px)', 'width:banana', 'background:url(image.png)', 'color:currentColor', 'color:var(color)', 'opacity:50%', 'height:100dvh', 'transform:translateX(blue)', 'transform:scale(10px)', 'decorator:ue-material(/Game/UI/M_Test)', 'decorator:var(--unknown)']) {
    assert.ok(errors(compileCss(`.x { ${declaration}; }`, strict)).length, declaration);
  }
  const variable = compileCss('.x { --accent:oklch(0.8 0.1 70); color:var(--accent,red); }', strict);
  assert.deepEqual(errors(variable), []);
  assert.ok(errors(compileCss('.x { --effect:linear-gradient(red,blue); decorator:var(--effect); }', strict)).some(item => item.code === 'unsupported-renderer-feature'));
  assert.ok(errors(compileCss('.x { --effect:rotateX(30deg); transform:var(--effect); }', strict)).some(item => item.code === 'unsupported-renderer-feature'));
});

test('inline WebCompat CSS goes through the same profile and points into original markup', () => {
  const css = '.x { border:1px solid #abc; color:rgba(1,2,3,.5); }';
  const direct = compileCss(css, strict);
  const markup = compileDocumentMarkup(`<html>\n<head><style>${css}</style></head><body/></html>`, strict);
  assert.ok(markup.markup.includes(direct.css));
  assert.throws(() => compileDocumentMarkup('<html>\n<head><style>.x { filter:blur(2px); }</style></head></html>', strict), /Panel.vue:2:/);
  assert.throws(() => compileDocumentMarkup('<html><body><div style="position:sticky"/></body></html>', strict), /unsupported-css-value/);
  assert.throws(() => compileDocumentMarkup('<html><body><div style="animation-play-state:paused"/></body></html>', strict),
    /unsupported-inline-motion-manifest/);
  assert.throws(() => compileDocumentMarkup('<html><head><link rel="stylesheet" href="bypass.css"/></head></html>', strict), /unvalidated-linked-stylesheet/);
  assert.doesNotThrow(() => compileDocumentMarkup('<html><body><div style="position:sticky"/></body></html>'));
});

test('the packaged Puerts compiler bundles and executes without Node or browser globals', async () => {
  const result = await build({ entryPoints: [new URL('../src/runtime-entry.mjs', import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1')], bundle: true, write: false, platform: 'browser', format: 'cjs', target: 'es2020', external: ['puerts'] });
  let callback, output;
  const bridge = { CapabilityProfile: 'slate-rhi', CapabilityMode: 'strict', AllowedDegradationsJson: '[]',
    OnCompileRequest: { Add(value) { callback = value; } }, ReportReady() {}, Complete(...args) { output = args; } };
  const sandbox = { module: { exports: {} }, exports: {}, require(name) { assert.equal(name, 'puerts'); return { argv: { getByName() { return bridge; } } }; } };
  vm.runInNewContext(result.outputFiles[0].text, sandbox);
  callback('<html><head><style>.x { --brand:#f00; color:var(--brand); }</style></head></html>', 'runtime.html');
  assert.equal(output[0], true);
  assert.match(output[1], /var\(--brand\)/);
  callback('<html><head><style>.x { filter:blur(3px); }</style></head></html>', 'runtime.html');
  assert.equal(output[0], false);
  assert.match(output[2], /unsupported-renderer-feature/);
});
