import { createRenderer, type ObjectDirective, type VNode } from '@vue/runtime-core';
import { native, check, report } from './bridge';
export * from '@vue/runtime-core';

export class RmlNode {
  props: Record<string, any> = {};
  events = new Map<string, number>();
  constructor(public handle: number, public tag = '') {}
}
export interface RmlEvent {
  type: string; target: RmlNode; currentTarget: RmlNode; value: string; checked: boolean;
  key: number; keyName: string; button: number; phase: number; x: number; y: number; modifiers: number;
  stopPropagation(): void; stopImmediatePropagation(): void;
  suppressTextInput(): void;
}
const nodes = new Map<number, RmlNode>();
const listeners = new Map<number, { node: RmlNode; callback: (event: RmlEvent) => void; once: boolean; key: string }>();
let sequence = 0;
function wrap(handle: number, tag = ''): RmlNode | null {
  if (!handle) return null;
  let node = nodes.get(handle);
  if (!node) { node = new RmlNode(handle, tag); nodes.set(handle, node); }
  return node;
}
function unlisten(node: RmlNode, key: string): void {
  const id = node.events.get(key);
  if (id !== undefined) { native.Unlisten(id); listeners.delete(id); node.events.delete(key); }
}
function listen(node: RmlNode, key: string, type: string, callback: (event: RmlEvent) => void, capture = false, once = false): void {
  unlisten(node, key);
  const id = ++sequence;
  check(native.Listen(node.handle, type, id, capture));
  node.events.set(key, id); listeners.set(id, { node, callback, once, key });
}
function dispatch(json: string): void {
  try {
    const data = JSON.parse(json), listener = listeners.get(data.listener);
    if (!listener) return;
    if (listener.once) unlisten(listener.node, listener.key);
    const event: RmlEvent = { ...data, target: wrap(data.target)!, currentTarget: wrap(data.currentTarget)!,
      stopPropagation: () => native.SetEventResult(1), stopImmediatePropagation: () => native.SetEventResult(2), suppressTextInput: () => native.SetEventResult(4) };
    listener.callback(event);
  } catch (error) { report(error); }
}
native.OnNativeEvent.Add(dispatch);
function sweep(): void {
  for (const [handle, node] of nodes) if (!native.IsNodeValid(handle)) {
    for (const key of [...node.events.keys()]) unlisten(node, key);
    nodes.delete(handle);
  }
}
export function disposeRenderer(): void {
  for (const node of nodes.values()) for (const key of [...node.events.keys()]) unlisten(node, key);
  nodes.clear(); listeners.clear(); native.OnNativeEvent.Remove(dispatch);
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
    const name = key === 'className' ? 'class' : key;
    check(native.SetAttribute(node.handle, name, next == null ? '' : String(next), next == null));
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
export const vModelText: ObjectDirective<RmlNode> = {
  created(node, binding, vnode) {
    node.props.modelVNode = vnode;
    listen(node, '$model', 'change', event => {
      let value: any = binding.modifiers.trim ? event.value.trim() : event.value;
      if (binding.modifiers.number) { const number = parseFloat(value); if (!Number.isNaN(number)) value = number; }
      assign(node.props.modelVNode, value);
    });
  },
  mounted(node, binding) { check(native.SetAttribute(node.handle, 'value', String(binding.value ?? ''), false)); },
  beforeUpdate(node, binding, vnode) {
    node.props.modelVNode = vnode;
    const value = String(binding.value ?? '');
    if (native.GetAttribute(node.handle, 'value') !== value) check(native.SetAttribute(node.handle, 'value', value, false));
  },
  beforeUnmount(node) { unlisten(node, '$model'); },
};
export const vModelCheckbox: ObjectDirective<RmlNode> = {
  created(node, _binding, vnode) {
    node.props.modelVNode = vnode;
    listen(node, '$model', 'change', event => assign(node.props.modelVNode, event.checked));
  },
  mounted(node, binding) { check(native.SetAttribute(node.handle, 'checked', String(!!binding.value), false)); },
  beforeUpdate(node, binding, vnode) {
    node.props.modelVNode = vnode;
    if (!!binding.value !== !!binding.oldValue) check(native.SetAttribute(node.handle, 'checked', String(!!binding.value), false));
  },
  beforeUnmount(node) { unlisten(node, '$model'); },
};
export const vModelSelect = vModelText;
export const vShow: ObjectDirective<RmlNode> = {
  beforeMount(node, binding) { check(native.SetProperty(node.handle, 'display', 'none', !!binding.value)); },
  updated(node, binding) { check(native.SetProperty(node.handle, 'display', 'none', !!binding.value)); },
};
export function withModifiers(fn: (event: RmlEvent) => void, modifiers: string[]) {
  return (event: RmlEvent) => {
    for (const modifier of modifiers) {
      if (modifier === 'stop') event.stopPropagation();
      else if (modifier === 'self' && event.target !== event.currentTarget) return;
      else if (modifier === 'ctrl' && !(event.modifiers & 2)) return;
      else if (modifier === 'shift' && !(event.modifiers & 1)) return;
      else if (modifier === 'alt' && !(event.modifiers & 4)) return;
      else if (!['self', 'ctrl', 'shift', 'alt'].includes(modifier)) throw new Error(`Unsupported event modifier: ${modifier}`);
    }
    fn(event);
  };
}
