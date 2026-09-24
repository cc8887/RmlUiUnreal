import { createRenderer, type ObjectDirective, type VNode } from '@vue/runtime-core';
import { native, check, report } from './bridge';
export * from '@vue/runtime-core';

export class RmlNode {
  props: Record<string, any> = {};
  events = new Map<string, number>();
  constructor(public handle: number, public tag = '') {}
  get isConnected(): boolean { return native.IsNodeValid(this.handle) && native.ContainsNode(native.RootNode(), this.handle); }
  get parentElement(): RmlNode | null { return wrap(native.ParentNode(this.handle)); }
  get childNodes(): RmlNode[] { return (JSON.parse(native.ChildNodes(this.handle)) as number[]).map(handle => wrap(handle)!); }
  getAttribute(name: string): string { return native.GetAttribute(this.handle, name); }
  setAttribute(name: string, value: string): void { check(native.SetAttribute(this.handle, name, value, false)); }
  removeAttribute(name: string): void { check(native.SetAttribute(this.handle, name, '', true)); }
  get classList() {
    const contains = (name: string) => this.getAttribute('class').split(/\s+/).includes(name);
    return {
      contains,
      add: (...names: string[]) => { for (const name of names) check(native.SetNodeClass(this.handle, name, true)); },
      remove: (...names: string[]) => { for (const name of names) check(native.SetNodeClass(this.handle, name, false)); },
      toggle: (name: string, force?: boolean) => { const enabled = force ?? !contains(name); check(native.SetNodeClass(this.handle, name, enabled)); return enabled; },
    };
  }
  querySelector(selector: string): RmlNode | null { return wrap(native.QueryNode(this.handle, selector)); }
  querySelectorAll(selector: string): RmlNode[] { return (JSON.parse(native.QueryNodes(this.handle, selector)) as number[]).map(handle => wrap(handle)!); }
  contains(node: RmlNode): boolean { return native.ContainsNode(this.handle, node.handle); }
  focus(): void { check(native.FocusNode(this.handle)); }
  blur(): void { check(native.BlurNode(this.handle)); }
  capturePointer(pointerId = 0): void { check(native.CaptureNode(this.handle, pointerId)); }
  releasePointer(pointerId = 0): void { check(native.ReleaseCaptureNode(this.handle, pointerId)); }
}
export interface RmlEvent {
  type: string; target: RmlNode; currentTarget: RmlNode; value: string; checked: boolean;
  key: number; keyName: string; button: number; phase: number; x: number; y: number; modifiers: number;
  code: string; repeat: boolean; buttons: number; pointerId: number; pointerType: string;
  relatedTarget: RmlNode | null; isComposing: boolean; data: string;
  wheelX: number; wheelY: number; timestamp: number; localX: number; localY: number;
  cancelable: boolean; defaultPrevented: boolean;
  stopPropagation(): void; stopImmediatePropagation(): void; preventDefault(): void;
  suppressTextInput(): void;
}
const nodes = new Map<number, RmlNode>();
const listeners = new Map<number, { node: RmlNode; callback: (event: RmlEvent) => void; once: boolean; key: string; type: string }>();
let sequence = 0;
function wrap(handle: number, tag = ''): RmlNode | null {
  if (!handle) return null;
  let node = nodes.get(handle);
  if (!node) { node = new RmlNode(handle, tag); nodes.set(handle, node); }
  return node;
}
export const wrapNode = wrap;
function unlisten(node: RmlNode, key: string): void {
  const id = node.events.get(key);
  if (id !== undefined) { native.Unlisten(id); listeners.delete(id); node.events.delete(key); }
}
function listen(node: RmlNode, key: string, type: string, callback: (event: RmlEvent) => void, capture = false, once = false): void {
  unlisten(node, key);
  const id = ++sequence;
  check(native.Listen(node.handle, type, id, capture));
  node.events.set(key, id); listeners.set(id, { node, callback, once, key, type });
}
export function onNodeEvent(node: RmlNode, type: string, callback: (event: RmlEvent) => void, capture = false): () => void {
  const key = `$subscription-${++sequence}`;
  listen(node, key, type, callback, capture);
  return () => unlisten(node, key);
}
function dispatch(json: string): void {
  try {
    const data = JSON.parse(json), listener = listeners.get(data.listener);
    if (!listener) return;
    if (listener.once) unlisten(listener.node, listener.key);
    const event: RmlEvent = { code: '', repeat: false, buttons: 0, pointerId: 0, pointerType: 'mouse', isComposing: false, data: '', wheelX: 0, wheelY: 0, timestamp: 0, localX: data.x, localY: data.y, cancelable: false, defaultPrevented: false,
      ...data, target: wrap(data.target)!, currentTarget: wrap(data.currentTarget)!, relatedTarget: wrap(data.relatedTarget || 0),
      stopPropagation: () => native.SetEventResult(1), stopImmediatePropagation: () => native.SetEventResult(2),
      preventDefault: () => { if (event.cancelable) { native.SetEventResult(8); event.defaultPrevented = true; } }, suppressTextInput: () => native.SetEventResult(4) };
    listener.callback(event);
  } catch (error) { report(error); }
}
native.OnNativeEvent.Add(dispatch);
function dispatchCssAnimationBatch(json: string): void {
  try {
    const batch = JSON.parse(json);
    if (batch?.type !== 'css-animation-events' || !Array.isArray(batch.events)) return;
    for (const data of batch.events) {
      let current = wrap(Number(data.target));
      while (current) {
        for (const listener of [...listeners.values()]) {
          if (listener.node !== current || listener.type !== data.type) continue;
          if (listener.once) unlisten(listener.node, listener.key);
          const event = { type: data.type, animationName: data.animationName, iteration: data.iteration,
            target: wrap(Number(data.target))!, currentTarget: current, value: '', checked: false,
            key: 0, keyName: '', button: 0, phase: current.handle === Number(data.target) ? 2 : 3,
            x: 0, y: 0, modifiers: 0, code: '', repeat: false, buttons: 0, pointerId: 0,
            pointerType: 'mouse', relatedTarget: null, isComposing: false, data: '', wheelX: 0, wheelY: 0,
            timestamp: 0, localX: 0, localY: 0, cancelable: false, defaultPrevented: false,
            stopPropagation: () => {}, stopImmediatePropagation: () => {}, preventDefault: () => {}, suppressTextInput: () => {} } as RmlEvent;
          listener.callback(event);
        }
        current = current.parentElement;
      }
    }
  } catch (error) { report(error); }
}
native.OnHostEvent?.Add(dispatchCssAnimationBatch);
function sweep(): void {
  for (const [handle, node] of nodes) if (!native.IsNodeValid(handle)) {
    for (const key of [...node.events.keys()]) unlisten(node, key);
    nodes.delete(handle);
  }
}
export function disposeRenderer(): void {
  for (const node of nodes.values()) for (const key of [...node.events.keys()]) unlisten(node, key);
  nodes.clear(); listeners.clear(); native.OnNativeEvent.Remove(dispatch); native.OnHostEvent?.Remove(dispatchCssAnimationBatch);
}
function kebab(name: string): string { return name.replace(/[A-Z]/g, char => '-' + char.toLowerCase()); }
function patchProp(node: RmlNode, key: string, previous: any, next: any): void {
  node.props[key] = next;
  if (key.startsWith('onUpdate:')) return;
  if (/^on[A-Z]/.test(key)) {
    unlisten(node, key);
    if (!next) return;
    const capture = key.endsWith('Capture') || key.includes('CaptureOnce');
    const once = key.includes('Once');
    const type = key.slice(2).replace(/(Once|Capture|Passive)/g, '').toLowerCase();
    listen(node, key, type === 'input' ? 'change' : type,
      event => { for (const callback of Array.isArray(next) ? next : [next]) callback(event); }, capture, once);
  } else if (key === 'style') {
    if (typeof next === 'string') throw new Error('Use a style object; static styles belong in RCSS.');
    const oldStyle = previous || {}, newStyle = next || {};
    for (const name of new Set([...Object.keys(oldStyle), ...Object.keys(newStyle)])) {
      const value = newStyle[name]; check(native.SetProperty(node.handle, kebab(name), value == null ? '' : String(value), value == null));
    }
  } else if (key === 'innerHTML') {
    throw new Error('v-html is not supported in a Vue-owned node tree.');
  } else if (key === 'textContent') {
    check(native.SetText(node.handle, next == null ? '' : String(next))); sweep();
  } else {
    if (node.tag === 'select' && key === 'multiple' && next != null && next !== false) throw new Error('Native select is single-value; use RmlMultiSelect for multiple values.');
    const name = key === 'className' ? 'class' : key === 'readOnly' ? 'readonly' : key;
    if (key === 'true-value' || key === 'false-value') return;
    const booleanAttribute = ['checked', 'selected', 'disabled', 'readonly', 'required', 'autofocus'].includes(name);
    check(native.SetAttribute(node.handle, name, next == null ? '' : String(next), next == null || (booleanAttribute && next === false)));
  }
}
const renderer = createRenderer<RmlNode, RmlNode>({
  createElement: tag => { const id = native.CreateNode(0, tag); check(id); return wrap(id, tag)!; },
  createText: text => { const id = native.CreateNode(1, text); check(id); return wrap(id, '#text')!; },
  createComment: () => { const id = native.CreateNode(2, ''); check(id); return wrap(id, '#comment')!; },
  setText: (node, text) => check(native.SetText(node.handle, text)),
  setElementText: (node, text) => { check(native.SetText(node.handle, text)); sweep(); },
  insert: (node, parent, anchor) => check(native.InsertNode(node.handle, parent.handle, anchor?.handle || 0)),
  remove: node => { check(native.RemoveNode(node.handle)); sweep(); },
  parentNode: node => wrap(native.ParentNode(node.handle)),
  nextSibling: node => wrap(native.NextNode(node.handle)),
  querySelector: selector => wrap(native.QueryNode(native.RootNode(), selector)),
  setScopeId: (node, id) => check(native.SetAttribute(node.handle, id, '', false)),
  patchProp,
});
export function createApp(component: any, props?: Record<string, unknown>) {
  const app = renderer.createApp(component, props);
  app.config.errorHandler = report;
  return { app, mount: () => app.mount(wrap(native.RootNode(), 'body')!), unmount: () => app.unmount() };
}
function assign(vnode: VNode, value: unknown): void {
  const handler = vnode.props?.['onUpdate:modelValue'];
  for (const callback of Array.isArray(handler) ? handler : [handler]) if (callback) callback(value);
}
function equalValue(a: unknown, b: unknown): boolean { return Object.is(a, b) || (a != null && b != null && typeof a !== 'object' && typeof b !== 'object' && String(a) === String(b)); }
function controlKind(node: RmlNode): string { return node.tag === 'select' ? 'select' : node.props.modelVNode?.props?.type || node.props.type || 'text'; }
function checkboxValue(node: RmlNode, checked: boolean): unknown {
  const key = checked ? 'true-value' : 'false-value';
  return Object.prototype.hasOwnProperty.call(node.props, key) ? node.props[key] : checked;
}
function commitModel(node: RmlNode, value: string, checked: boolean): void {
  const binding = node.props.modelBinding;
  if (!binding) return;
  const kind = controlKind(node), vnode = node.props.modelVNode;
  const option = Object.prototype.hasOwnProperty.call(node.props, 'value') ? node.props.value : value;
  if (kind === 'checkbox') {
    if (Array.isArray(binding.value)) {
      const next = binding.value.filter((item: unknown) => !equalValue(item, option));
      if (checked) next.push(option);
      assign(vnode, next);
    } else if (binding.value instanceof Set) {
      const next = new Set(binding.value);
      if (checked) next.add(option); else for (const item of next) if (equalValue(item, option)) next.delete(item);
      assign(vnode, next);
    } else assign(vnode, checkboxValue(node, checked));
  } else if (kind === 'radio') { if (checked) assign(vnode, option); }
  else {
    let result: string | number = binding.modifiers.trim ? value.trim() : value;
    if (binding.modifiers.number || kind === 'number') { const number = parseFloat(result); if (!Number.isNaN(number)) result = number; }
    assign(vnode, result);
  }
}
function syncModel(node: RmlNode): void {
  const binding = node.props.modelBinding;
  if (!binding || node.props.composing) return;
  const kind = controlKind(node), value = binding.value;
  if (kind === 'radio' || kind === 'checkbox') {
    const option = node.props.value;
    const checked = kind === 'radio' ? equalValue(value, option) :
      Array.isArray(value) || value instanceof Set ? [...value].some(item => equalValue(item, option)) : equalValue(value, checkboxValue(node, true));
    if ((native.GetAttribute(node.handle, 'checked') === 'true') !== checked) check(native.SetAttribute(node.handle, 'checked', String(checked), false));
  } else {
    if (kind === 'select' && node.props.multiple != null && node.props.multiple !== false) throw new Error('Native select is single-value; use RmlMultiSelect.');
    if (binding.modifiers.lazy && native.ActiveNode() === node.handle && equalValue(binding.value, binding.oldValue)) return;
    const text = String(value ?? '');
    if (native.GetAttribute(node.handle, 'value') !== text) check(native.SetAttribute(node.handle, 'value', text, false));
  }
}
const modelDirective: ObjectDirective<RmlNode> = {
  created(node, binding, vnode) {
    node.props.modelVNode = vnode; node.props.modelBinding = binding;
    const commit = () => commitModel(node, native.GetAttribute(node.handle, 'value'), native.GetAttribute(node.handle, 'checked') === 'true');
    listen(node, '$model', 'change', event => {
      if (node.props.composing || event.isComposing) return;
      if (node.props.modelBinding.modifiers.lazy && !['checkbox', 'radio', 'select'].includes(controlKind(node))) return;
      commitModel(node, event.value, event.checked);
    });
    listen(node, '$model-composition-start', 'compositionstart', () => { node.props.composing = true; });
    listen(node, '$model-composition-end', 'compositionend', () => {
      node.props.composing = false;
      if (!node.props.modelBinding.modifiers.lazy) commit();
    });
    listen(node, '$model-blur', 'blur', () => { if (!node.props.composing && node.props.modelBinding.modifiers.lazy) commit(); });
    listen(node, '$model-enter', 'keydown', event => {
      if (['checkbox', 'radio'].includes(controlKind(node)) && [' ', 'Space', 'Spacebar'].includes(event.keyName) && !event.repeat && !event.isComposing && native.GetAttribute(node.handle, 'disabled') !== 'true') {
        event.preventDefault();
        const checked = controlKind(node) === 'radio' || native.GetAttribute(node.handle, 'checked') !== 'true';
        check(native.SetAttribute(node.handle, 'checked', String(checked), false));
        return;
      }
      if (event.keyName === 'Enter' && !event.isComposing && !node.props.composing && node.props.modelBinding.modifiers.lazy) commit();
    });
  },
  mounted: syncModel,
  beforeUpdate(node, binding, vnode) { node.props.modelVNode = vnode; node.props.modelBinding = binding; },
  updated: syncModel,
  beforeUnmount(node) { for (const key of [...node.events.keys()]) if (key.startsWith('$model')) unlisten(node, key); },
};
export const vModelText = modelDirective;
export const vModelCheckbox = modelDirective;
export const vModelRadio = modelDirective;
export const vModelSelect = modelDirective;
export const vModelDynamic = modelDirective;
export const vShow: ObjectDirective<RmlNode> = {
  beforeMount(node, binding) { check(native.SetProperty(node.handle, 'display', 'none', !!binding.value)); },
  updated(node, binding) { check(native.SetProperty(node.handle, 'display', 'none', !!binding.value)); },
};
export function withModifiers(fn: (event: RmlEvent) => void, modifiers: string[]) {
  return (event: RmlEvent) => {
    for (const modifier of modifiers) {
      if (modifier === 'stop') event.stopPropagation();
      else if (modifier === 'prevent') event.preventDefault();
      else if (modifier === 'self' && event.target !== event.currentTarget) return;
      else if (modifier === 'ctrl' && !(event.modifiers & 2)) return;
      else if (modifier === 'shift' && !(event.modifiers & 1)) return;
      else if (modifier === 'alt' && !(event.modifiers & 4)) return;
      else if (modifier === 'meta' && !(event.modifiers & 32)) return;
      else if (modifier === 'exact' && ([['ctrl', 2], ['shift', 1], ['alt', 4], ['meta', 32]] as const).some(([name, bit]) => !modifiers.includes(name) && (event.modifiers & bit))) return;
      else if (!['self', 'ctrl', 'shift', 'alt', 'meta', 'exact'].includes(modifier)) throw new Error(`Unsupported event modifier: ${modifier}`);
    }
    fn(event);
  };
}
export function withKeys(fn: (event: RmlEvent) => void, keys: string[]) {
  const aliases: Record<string, string[]> = { enter: ['Enter'], esc: ['Escape'], tab: ['Tab'], space: [' ', 'Space'], up: ['ArrowUp'], down: ['ArrowDown'], left: ['ArrowLeft'], right: ['ArrowRight'], delete: ['Backspace', 'Delete'] };
  return (event: RmlEvent) => {
    if (event.isComposing) return;
    if (keys.some(key => (aliases[key] || [key]).some(name => name.toLowerCase() === event.keyName.toLowerCase()))) fn(event);
  };
}
