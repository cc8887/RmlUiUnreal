import { check, native, report } from './bridge';

export interface NodeMetrics {
  handle: number; visible: boolean; x: number; y: number; width: number; height: number;
  layoutX: number; layoutY: number; layoutWidth: number; layoutHeight: number;
  scrollTop: number; scrollLeft: number; scrollWidth: number; scrollHeight: number;
  clientWidth: number; clientHeight: number;
  clipX: number; clipY: number; clipWidth: number; clipHeight: number;
}
export interface LayoutSnapshot {
  revision: number;
  viewport: { width: number; height: number; dpi: number };
  nodes: NodeMetrics[];
}
type Cleanup = () => void;
interface Observation { handles: () => number[]; callback: (snapshot: LayoutSnapshot) => void | Promise<void>; signature: string }
const observations = new Set<Observation>();
const ready = new Set<() => void>();
let disposed = false;
let overlayRoot = 0;

export function queryNode(selector: string, root = native.RootNode()): number {
  return native.QueryNode(root, selector);
}
export function queryNodes(selector: string, root = native.RootNode()): number[] {
  return JSON.parse(native.QueryNodes(root, selector));
}
export function childrenOf(node: number): number[] { return JSON.parse(native.ChildNodes(node)); }
export function containsNode(parent: number, child: number): boolean {
  return !!parent && !!child && native.ContainsNode(parent, child);
}
/** Reads the last completed layout. Mutations are observed after the next native layout. */
export function measureNodes(handles: number[]): LayoutSnapshot {
  // The native batch already omits detached/stale handles. Avoid a separate
  // native validity call per node so observers retain one bridge call per batch.
  return JSON.parse(native.MeasureNodes(JSON.stringify([...new Set(handles)].filter(handle => Number.isInteger(handle) && handle > 0))));
}
export function afterLayout(callback: () => void): Cleanup {
  if (!disposed) ready.add(callback);
  return () => ready.delete(callback);
}
/** Layout and scroll observations share one native measurement per completed frame. */
export function observeLayout(handles: () => number[], callback: (snapshot: LayoutSnapshot) => void | Promise<void>): Cleanup {
  const observation = { handles, callback, signature: '' };
  if (!disposed) observations.add(observation);
  return () => observations.delete(observation);
}
function layoutCompleted(): void {
  if (disposed) return;
  const pending = [...ready]; ready.clear();
  for (const callback of pending) { try { callback(); } catch (error) { report(error); } }
  const active = [...observations];
  if (!active.length) return;
  const snapshot = measureNodes(active.flatMap(item => item.handles()));
  for (const item of active) {
    if (!observations.has(item)) continue;
    const handles = new Set(item.handles());
    const selection = { ...snapshot, nodes: snapshot.nodes.filter(node => handles.has(node.handle)) };
    const signature = JSON.stringify([selection.viewport, selection.nodes]);
    if (signature === item.signature) continue;
    item.signature = signature;
    try { const result = item.callback(selection); if (result && typeof result.catch === 'function') result.catch(report); } catch (error) { report(error); }
  }
}
export function setTheme(tokens: Record<string, string>, root = native.RootNode()): void {
  for (const [name, value] of Object.entries(tokens)) {
    if (!/^--[a-zA-Z0-9_-]+$/.test(name)) throw new Error(`Invalid theme token: ${name}`);
    check(native.SetProperty(root, name, value, false));
  }
}
/** A View-owned native target; this is deliberately not a browser document facade. */
export function ensureOverlayRoot(): number {
  if (overlayRoot && native.IsNodeValid(overlayRoot)) return overlayRoot;
  overlayRoot = native.CreateNode(0, 'div'); check(overlayRoot);
  check(native.SetAttribute(overlayRoot, 'id', 'rml-overlay-root', false));
  for (const [name, value] of Object.entries({ position: 'fixed', top: '0px', left: '0px', width: '100%', height: '100%', 'z-index': '1000', 'pointer-events': 'none' })) {
    check(native.SetProperty(overlayRoot, name, value, false));
  }
  check(native.InsertNode(overlayRoot, native.RootNode(), 0));
  return overlayRoot;
}
export function disposePlatform(): void {
  if (disposed) return;
  disposed = true; observations.clear(); ready.clear();
  native.OnAfterLayout.Remove(layoutCompleted);
  if (overlayRoot && native.IsNodeValid(overlayRoot)) native.RemoveNode(overlayRoot);
  overlayRoot = 0;
}
native.OnAfterLayout.Add(layoutCompleted);
