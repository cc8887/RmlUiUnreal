import test from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { readFile } from 'node:fs/promises';
import { build } from 'esbuild';
import { parse, compileScript, compileTemplate } from '@vue/compiler-sfc';

function delegate() {
  const handlers = new Set();
  return { Add: callback => handlers.add(callback), Remove: callback => handlers.delete(callback), emit: (...args) => { for (const callback of [...handlers]) callback(...args); }, get size() { return handlers.size; } };
}
function nativeFixture() {
  let next = 1, active = 0, results = 0, measurements = 0, validityChecks = 0;
  const records = new Map([[1, { tag: 'body', attrs: {}, properties: {}, children: [], parent: 0, metrics: {} }]]);
  const listeners = new Map(), modalCalls = [], measurementBatches = [];
  const native = {
    LastError: 'Invalid native operation', OnNativeEvent: delegate(), OnAfterLayout: delegate(), OnLifecycle: delegate(),
    RootNode: () => 1, IsNodeValid: handle => { validityChecks++; return records.has(handle); }, FindNode: id => [...records].find(([, node]) => node.attrs.id === id)?.[0] || 0,
    CreateNode: (kind, text) => { const handle = ++next; records.set(handle, { tag: kind ? '#text' : text, text: kind ? text : '', attrs: {}, properties: {}, children: [], parent: 0, metrics: {} }); return handle; },
    InsertNode: (handle, parent, before) => {
      const node = records.get(handle), target = records.get(parent); if (!node || !target) return false;
      if (node.parent) { const siblings = records.get(node.parent).children; siblings.splice(siblings.indexOf(handle), 1); }
      const at = before ? target.children.indexOf(before) : target.children.length;
      target.children.splice(at, 0, handle); node.parent = parent; return true;
    },
    RemoveNode: handle => {
      const node = records.get(handle); if (!node) return false;
      for (const child of [...node.children]) native.RemoveNode(child);
      if (node.parent) { const siblings = records.get(node.parent)?.children; const at = siblings?.indexOf(handle); if (at >= 0) siblings.splice(at, 1); }
      records.delete(handle); return true;
    },
    ParentNode: handle => records.get(handle)?.parent || 0,
    NextNode: handle => { const parent = records.get(records.get(handle)?.parent); return parent?.children[parent.children.indexOf(handle) + 1] || 0; },
    ChildNodes: handle => JSON.stringify(records.get(handle)?.children || []),
    ContainsNode: (parent, child) => { for (let handle = child; handle; handle = records.get(handle)?.parent || 0) if (handle === parent) return true; return false; },
    QueryNode: (root, selector) => JSON.parse(native.QueryNodes(root, selector))[0] || 0,
    QueryNodes: (root, selector) => JSON.stringify([...records].filter(([handle, node]) => native.ContainsNode(root, handle) && handle !== root && (selector === '*' || selector === node.tag || (selector[0] === '#' && node.attrs.id === selector.slice(1)))).map(([handle]) => handle)),
    SetAttribute: (handle, name, value, remove) => { const node = records.get(handle); if (!node) return false; if (remove) delete node.attrs[name]; else node.attrs[name] = value; if (name === 'checked') event(handle, 'change', { checked: !remove && value !== 'false' && value !== '0' }); return true; },
    GetAttribute: (handle, name) => records.get(handle)?.attrs[name] || '',
    SetProperty: (handle, name, value, remove) => { const node = records.get(handle); if (!node) return false; if (remove) delete node.properties[name]; else node.properties[name] = value; return true; },
    GetComputedProperty: (handle, name) => records.get(handle)?.properties[name] || '',
    SetText: (handle, text) => { records.get(handle).text = text; return true; }, GetText: handle => records.get(handle)?.text || '',
    Listen: (handle, type, id, capture) => { listeners.set(id, { handle, type, capture }); return true; }, Unlisten: id => listeners.delete(id),
    SetEventResult: result => { results |= result; }, ReportError: message => { throw new Error(message); },
    ActiveNode: () => active, FocusNode: handle => { active = handle; return true; }, BlurNode: () => { active = 0; return true; },
    SetModalRoot: (root, initial) => { modalCalls.push([root, initial]); if (initial) active = initial; return true; },
    MeasureNodes: json => {
      measurements++;
      measurementBatches.push(JSON.parse(json));
      return JSON.stringify({ revision: measurements, viewport: { width: 800, height: 600, dpi: 1 }, nodes: JSON.parse(json).map(handle => ({ handle, visible: true, x: 0, y: 0, width: 100, height: 30, layoutX: 0, layoutY: 0, layoutWidth: 100, layoutHeight: 30, scrollTop: 0, scrollLeft: 0, scrollWidth: 100, scrollHeight: 30, clientWidth: 100, clientHeight: 30, clipX: 0, clipY: 0, clipWidth: 800, clipHeight: 600, ...records.get(handle).metrics })) });
    },
  };
  function event(handle, type, fields = {}) {
    results = 0;
    if ('value' in fields) records.get(handle).attrs.value = fields.value;
    if ('checked' in fields) records.get(handle).attrs.checked = String(fields.checked);
    for (const [listener, entry] of [...listeners]) if (entry.handle === handle && entry.type === type) {
      native.OnNativeEvent.emit(JSON.stringify({ listener, type, target: handle, currentTarget: handle, value: native.GetAttribute(handle, 'value'), checked: native.GetAttribute(handle, 'checked') === 'true', keyName: '', modifiers: 0, cancelable: true, ...fields }));
    }
    return results;
  }
  function dispatch(handle, type, fields = {}, nativeTarget = () => {}) {
    results = 0;
    const ancestors = [];
    for (let parent = native.ParentNode(handle); parent; parent = native.ParentNode(parent)) ancestors.push(parent);
    const snapshot = [...listeners];
    const invoke = (currentTarget, capture, phase) => {
      for (const [listener, entry] of snapshot) {
        if ((results & 2) || entry.handle !== currentTarget || entry.type !== type || entry.capture !== capture) continue;
        native.OnNativeEvent.emit(JSON.stringify({ listener, type, target: handle, currentTarget, phase, keyName: '', modifiers: 0, cancelable: true, ...fields }));
      }
    };
    // RmlUi collects the physical parent chain before dispatch. Its text input target
    // listener stops even unrecognized keys such as Escape before the bubble phase.
    for (const parent of [...ancestors].reverse()) { invoke(parent, true, 1); if (results & 3) return results; }
    invoke(handle, true, 2); invoke(handle, false, 2);
    if (!(results & 2)) nativeTarget(native);
    if (!(results & 3)) for (const parent of ancestors) { invoke(parent, false, 3); if (results & 3) break; }
    return results;
  }
  return { native, records, listeners, modalCalls, measurementBatches, event, dispatch, get measurements() { return measurements; }, get validityChecks() { return validityChecks; } };
}
let sequence = 0;
async function load(fixture, modules) {
  const key = `__rmlTest${++sequence}`; globalThis[key] = fixture.native;
  const output = await build({ stdin: { contents: modules.map(([alias, entry]) => `export * as ${alias} from ${JSON.stringify(entry)};`).join('\n'), resolveDir: path.resolve('.') }, bundle: true, write: false, platform: 'node', format: 'esm',
    plugins: [{ name: 'native-fixture', setup(builder) {
      builder.onResolve({ filter: /^(vue|@rmlui\/vue)$/ }, () => ({ path: path.resolve('src/renderer.ts') }));
      builder.onLoad({ filter: /[/\\]src[/\\]bridge\.ts$/ }, () => ({ contents: `export const native=globalThis[${JSON.stringify(key)}]; export function check(value){if(!value)throw Error(native.LastError)}; export function report(error){native.ReportError(String(error))}`, loader: 'ts' }));
      builder.onLoad({ filter: /\.vue$/ }, async ({ path: filename }) => {
        const { descriptor, errors } = parse(await readFile(filename, 'utf8'), { filename });
        assert.deepEqual(errors, []);
        const id = path.basename(filename), script = compileScript(descriptor, { id, genDefaultAs: '__component' });
        const template = compileTemplate({ source: descriptor.template.content, filename, id,
          compilerOptions: { runtimeModuleName: '@rmlui/vue', bindingMetadata: script.bindings, hoistStatic: false } });
        assert.deepEqual(template.errors, []);
        return { contents: `${script.content}\n${template.code}\n__component.render=render; export default __component;`, loader: 'ts', resolveDir: path.dirname(filename) };
      });
    } }],
  });
  return import(`data:text/javascript;base64,${Buffer.from(output.outputFiles[0].text).toString('base64')}`);
}

test('native host observes one batched layout, suppresses unchanged callbacks and disposes cleanly', async () => {
  const f = nativeFixture(); const { platform } = await load(f, [['platform', './src/platform.ts']]);
  const a = f.native.CreateNode(0, 'div'), b = f.native.CreateNode(0, 'div');
  let callsA = 0, callsB = 0, ready = 0;
  platform.afterLayout(() => ready++);
  const cancelA = platform.observeLayout(() => [a, a, 0, -1, 1.5, NaN, Infinity], () => callsA++);
  platform.observeLayout(() => [b], () => callsB++);
  f.native.OnAfterLayout.emit(1); assert.equal(f.measurements, 1); assert.deepEqual([callsA, callsB, ready], [1, 1, 1]);
  assert.deepEqual(f.measurementBatches, [[a, b]], 'observers share one deduplicated native measurement batch');
  assert.equal(f.validityChecks, 0, 'batch measurement must not perform per-node native validity calls');
  f.native.OnAfterLayout.emit(2); assert.deepEqual([callsA, callsB, ready], [1, 1, 1]);
  f.records.get(a).metrics.scrollTop = 12; f.native.OnAfterLayout.emit(3); assert.deepEqual([callsA, callsB], [2, 1]);
  assert.equal(f.measurements, 3); assert.equal(f.validityChecks, 0);
  cancelA(); platform.setTheme({ '--accent': '#aabbcc' }); assert.equal(f.records.get(1).properties['--accent'], '#aabbcc');
  assert.throws(() => platform.setTheme({ color: '#fff' }), /Invalid theme token/);
  const overlay = platform.ensureOverlayRoot(); assert.equal(platform.ensureOverlayRoot(), overlay); assert.equal(f.native.ParentNode(overlay), 1);
  platform.disposePlatform(); assert.equal(f.native.OnAfterLayout.size, 0); assert.equal(f.native.IsNodeValid(overlay), false);
  f.native.OnAfterLayout.emit(4); assert.equal(f.measurements, 3); assert.deepEqual([callsA, callsB, ready], [2, 1, 1]);
});

test('Vue native model binds arrays, Sets, radios, lazy values and composition without browser globals', async () => {
  const f = nativeFixture(); const { vue } = await load(f, [['vue', './src/renderer.ts']]);
  const values = vue.ref(['layout']), flags = vue.ref(new Set(['inspect'])), mode = vue.ref('translate'), text = vue.ref(''), lazy = vue.ref('initial');
  const field = (id, type, value, model, directive, modifiers = {}) => vue.withDirectives(vue.h('input', { id, type, value, 'onUpdate:modelValue': next => { model.value = next; } }), [[directive, model.value, undefined, modifiers]]);
  const instance = vue.createApp({ setup: () => () => vue.h('div', [field('array', 'checkbox', 'render', values, vue.vModelCheckbox), field('set', 'checkbox', 'edit', flags, vue.vModelCheckbox), field('radio', 'radio', 'rotate', mode, vue.vModelRadio), field('ime', 'text', undefined, text, vue.vModelText), field('lazy', 'text', undefined, lazy, vue.vModelText, { lazy: true, trim: true })]) });
  instance.mount();
  f.event(f.native.FindNode('array'), 'change', { checked: true }); await vue.nextTick(); assert.deepEqual([...values.value], ['layout', 'render']);
  f.event(f.native.FindNode('array'), 'change', { checked: false }); await vue.nextTick(); assert.deepEqual([...values.value], ['layout']);
  f.event(f.native.FindNode('set'), 'change', { checked: true }); await vue.nextTick(); assert.deepEqual([...flags.value], ['inspect', 'edit']);
  f.event(f.native.FindNode('set'), 'keydown', { keyName: 'Space' }); await vue.nextTick(); assert.deepEqual([...flags.value], ['inspect']);
  f.event(f.native.FindNode('radio'), 'change', { checked: true }); await vue.nextTick(); assert.equal(mode.value, 'rotate');
  const ime = f.native.FindNode('ime'); f.event(ime, 'compositionstart'); f.event(ime, 'change', { value: '候选', isComposing: true }); await vue.nextTick(); assert.equal(text.value, '');
  f.native.SetAttribute(ime, 'value', '中文', false); f.event(ime, 'compositionend'); await vue.nextTick(); assert.equal(text.value, '中文');
  const delayed = f.native.FindNode('lazy'); f.event(delayed, 'change', { value: '  saved  ' }); await vue.nextTick(); assert.equal(lazy.value, 'initial');
  f.event(delayed, 'blur'); await vue.nextTick(); assert.equal(lazy.value, 'saved');
  instance.unmount(); vue.disposeRenderer(); assert.equal(f.listeners.size, 0); assert.equal(f.native.OnNativeEvent.size, 0);
});

test('Teleport preserves node identity and native preventDefault differs from propagation', async () => {
  const f = nativeFixture(); const { vue } = await load(f, [['vue', './src/renderer.ts']]);
  const portal = f.native.CreateNode(0, 'div'); f.native.SetAttribute(portal, 'id', 'portal', false); f.native.InsertNode(portal, 1, 0);
  const target = vue.ref('#portal'); let prevented = false;
  const instance = vue.createApp({ setup: () => () => vue.h('div', { id: 'shell' }, [vue.h(vue.Teleport, { to: target.value }, [vue.h('button', { id: 'teleported', onClick: vue.withModifiers(event => { prevented = event.defaultPrevented; }, ['prevent']) }, 'Panel')])]) });
  instance.mount(); const node = f.native.FindNode('teleported'); assert.equal(f.native.ParentNode(node), portal);
  const flags = f.event(node, 'click'); assert.equal(flags, 8); assert.equal(prevented, true);
  const second = f.native.CreateNode(0, 'div'); f.native.SetAttribute(second, 'id', 'second', false); f.native.InsertNode(second, 1, 0);
  target.value = '#second'; await vue.nextTick(); assert.equal(f.native.FindNode('teleported'), node); assert.equal(f.native.ParentNode(node), second);
  instance.unmount(); vue.disposeRenderer(); assert.equal(f.native.IsNodeValid(node), false);
});

test('native modal stack restores the parent and removes a non-top modal without stealing scope', async () => {
  const f = nativeFixture(); const { manager, platform } = await load(f, [['manager', './src/dialogs/dialogManager.ts'], ['platform', './src/platform.ts']]);
  const outer = f.native.CreateNode(0, 'div'), inner = f.native.CreateNode(0, 'div'), trigger = f.native.CreateNode(0, 'button'), input = f.native.CreateNode(0, 'input');
  f.native.InsertNode(outer, 1, 0); f.native.InsertNode(inner, 1, 0); f.native.InsertNode(trigger, outer, 0); f.native.InsertNode(input, inner, 0);
  const closeOuter = manager.pushModal(outer, trigger, 0); f.native.OnAfterLayout.emit(1);
  const closeInner = manager.pushModal(inner, input, trigger); f.native.OnAfterLayout.emit(2);
  assert.equal(manager.isTopModal(inner), true); closeInner(); assert.deepEqual(f.modalCalls.at(-1), [outer, trigger]);
  const closeAgain = manager.pushModal(inner, input, trigger); closeOuter(); assert.equal(manager.isTopModal(inner), true); closeAgain(); assert.deepEqual(f.modalCalls.at(-1), [0, 0]);
  platform.disposePlatform();
});

test('nested RmlDialog Escape precedes native text handling, closes only the top and restores focus', async () => {
  const f = nativeFixture();
  const { vue, dialog, manager, platform } = await load(f, [['vue', './src/renderer.ts'], ['dialog', './src/dialogs/RmlDialog.vue'], ['manager', './src/dialogs/dialogManager.ts'], ['platform', './src/platform.ts']]);
  const outerOpen = vue.ref(true), innerOpen = vue.ref(false);
  let outerCloses = 0, innerCloses = 0;
  const instance = vue.createApp({ setup: () => () => vue.h('div', [
    vue.h('button', { id: 'outer-trigger' }, 'Outer'),
    outerOpen.value ? vue.h(dialog.default, { dialogId: 'outer', titleId: 'outer-title', initialFocusId: 'outer-input', triggerId: 'outer-trigger', onClose: () => { outerCloses++; outerOpen.value = false; } }, {
      default: () => [vue.h('input', { id: 'outer-input', type: 'text' }), vue.h('button', { id: 'inner-trigger' }, 'Inner'),
        innerOpen.value ? vue.h(dialog.default, { dialogId: 'inner', titleId: 'inner-title', initialFocusId: 'inner-input', triggerId: 'inner-trigger', onClose: () => { innerCloses++; innerOpen.value = false; } }, {
          default: () => [vue.h('input', { id: 'inner-input', type: 'text' }), vue.h('button', { id: 'inner-close' }, 'Close')],
        }) : null],
    }) : null,
  ]) });
  instance.mount(); f.native.OnAfterLayout.emit(1);
  innerOpen.value = true; await vue.nextTick(); f.native.OnAfterLayout.emit(2);
  const input = f.native.FindNode('inner-input'), inner = f.native.FindNode('inner-overlay'), outer = f.native.FindNode('outer-overlay');
  assert.equal(f.native.ActiveNode(), input); assert.equal(manager.modalDepth(), 2);
  assert.equal(f.native.ParentNode(inner), f.native.ParentNode(outer), 'nested portals are physical siblings');
  const textTarget = native => native.SetEventResult(1);
  f.dispatch(input, 'keydown', { keyName: 'Escape', isComposing: true }, textTarget); await vue.nextTick();
  assert.deepEqual([outerCloses, innerCloses], [0, 0], 'IME Escape must remain with text composition');
  const result = f.dispatch(input, 'keydown', { keyName: 'Escape' }, textTarget); await vue.nextTick();
  assert.equal(result, 10, 'capture stops the event and prevents its native default before text handling');
  assert.deepEqual([outerCloses, innerCloses], [0, 1], 'one Escape dismisses only the inner dialog');
  assert.equal(f.native.IsNodeValid(inner), false); assert.equal(f.native.IsNodeValid(outer), true);
  assert.equal(f.native.ActiveNode(), f.native.FindNode('inner-trigger')); assert.equal(manager.modalDepth(), 1);
  f.dispatch(f.native.ActiveNode(), 'keydown', { keyName: 'Escape' }); await vue.nextTick();
  assert.deepEqual([outerCloses, innerCloses], [1, 1]); assert.equal(f.native.ActiveNode(), f.native.FindNode('outer-trigger'));
  instance.unmount(); platform.disposePlatform(); vue.disposeRenderer(); assert.equal(f.listeners.size, 0); assert.equal(manager.modalDepth(), 0);
});

test('Floating UI core flips a native popover and hides a clipped anchor', async () => {
  const f = nativeFixture(); const { floating } = await load(f, [['floating', './src/dialogs/floatingPlatform.ts']]);
  const anchor = { handle: 1, x: 720, y: 560, width: 60, height: 28, clipX: 0, clipY: 0, clipWidth: 800, clipHeight: 600 };
  const panel = { handle: 2, x: 0, y: 0, width: 268, height: 160, clipX: 0, clipY: 0, clipWidth: 800, clipHeight: 600 };
  const snapshot = { revision: 1, viewport: { width: 800, height: 600, dpi: 1 }, nodes: [anchor, panel] };
  const result = await floating.positionPopover(snapshot, 1, 2); assert.match(result.placement, /^top/); assert.ok(result.x >= 12 && result.x + 268 <= 788); assert.ok(result.y < 560);
  anchor.clipHeight = 100; const hidden = await floating.positionPopover(snapshot, 1, 2); assert.equal(hidden.middlewareData.hide.referenceHidden, true);
});

test('RmlPopover captures Escape from a slotted native text input and restores its anchor', async () => {
  const f = nativeFixture();
  const { vue, popover, platform } = await load(f, [['vue', './src/renderer.ts'], ['popover', './src/dialogs/RmlPopover.vue'], ['platform', './src/platform.ts']]);
  const open = vue.ref(true); let closes = 0;
  const instance = vue.createApp({ setup: () => () => vue.h('div', [
    vue.h('button', { id: 'anchor' }, 'Open'),
    open.value ? vue.h(popover.default, { popoverId: 'popover', anchorId: 'anchor', onClose: () => { closes++; open.value = false; } }, {
      default: () => vue.h('input', { id: 'popover-input', type: 'text' }),
    }) : null,
  ]) });
  instance.mount(); const input = f.native.FindNode('popover-input'); f.native.FocusNode(input);
  const textTarget = native => native.SetEventResult(1);
  f.dispatch(input, 'keydown', { keyName: 'Escape', isComposing: true }, textTarget); await vue.nextTick(); assert.equal(closes, 0);
  assert.equal(f.dispatch(input, 'keydown', { keyName: 'Escape' }, textTarget), 10); await vue.nextTick();
  assert.equal(closes, 1); assert.equal(f.native.FindNode('popover'), 0); assert.equal(f.native.ActiveNode(), f.native.FindNode('anchor'));
  instance.unmount(); platform.disposePlatform(); vue.disposeRenderer(); assert.equal(f.listeners.size, 0);
});
