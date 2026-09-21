export interface Delegate<T extends (...args: any[]) => void> { Add(callback: T): void; Remove(callback: T): void }
export interface NativeBridge {
  StateJson: string; Version: string; LastError: string;
  RootNode(): number; FindNode(id: string): number; CreateNode(kind: number, text: string): number;
  IsNodeValid(node: number): boolean; InsertNode(node: number, parent: number, before: number): boolean;
  RemoveNode(node: number): boolean; ParentNode(node: number): number; NextNode(node: number): number;
  SetText(node: number, text: string): boolean; GetText(node: number): string;
  SetAttribute(node: number, name: string, value: string, remove: boolean): boolean;
  GetAttribute(node: number, name: string): string;
  SetProperty(node: number, name: string, value: string, remove: boolean): boolean;
  SetInnerRml(node: number, rml: string): boolean;
  Listen(node: number, type: string, listener: number, capture: boolean): boolean; Unlisten(listener: number): void;
  SetEventResult(result: number): void; SaveState(json: string): void; ReportReady(): void;
  ReportError(message: string): void; ReportDebugState(json: string): void;
  SetWakeSchedule(timerDelayMilliseconds: number, hasAnimationFrame: boolean): void;
  RequestHost(id: number, method: string, json: string): void;
  ScrollNode(node: number, top: number): boolean; ScrollRemaining(node: number): number; FocusNode(node: number): boolean;
  QueryNode(root: number, selector: string): number; QueryNodes(root: number, selector: string): string;
  ChildNodes(node: number): string; ContainsNode(parent: number, child: number): boolean;
  ActiveNode(): number; BlurNode(node: number): boolean; SetNodeClass(node: number, name: string, enabled: boolean): boolean;
  GetComputedProperty(node: number, name: string): string; MeasureNodes(handlesJson: string): string;
  ResolveAnimationHostSnapshot(requestJson: string): string;
  SetModalRoot(root: number, initialFocus: number): boolean;
  CaptureNode(node: number, pointerId: number): boolean; ReleaseCaptureNode(node: number, pointerId: number): boolean;
  AnimateNode(node: number, property: string, from: string, to: string, duration: number, iterations: number): boolean;
  AnimateNodeKeyframes(node: number, property: string, keyframesJson: string, duration: number, iterations: number): boolean;
  StartNodeKeyframeAnimation(node: number, property: string, keyframesJson: string, optionsJson: string): string;
  StartNodeKeyframeAnimationBatch(requestsJson: string): string;
  RegisterAnimationPlansPacked?(payload: ArrayBuffer): string;
  StartCompiledAnimationBatchPacked?(payload: ArrayBuffer): string;
  ReleaseAnimationPlansPacked?(payload: ArrayBuffer): string;
  GetAnimationPlanCacheStats?(): string;
  ControlAnimation(handle: string, command: string, value: number): string;
  ApplyNodePropertyBatch(updatesJson: string): string;
  CancelAnimation(node: number, property: string): boolean;
  bUsePackedAnimationEvents: boolean;
  bUseCompiledAnimationPlans?: boolean;
  CompiledAnimationPlanCacheMaxEntries?: number;
  CompiledAnimationPlanCacheMaxBytes?: number;
  bShareAnimationDefinitionsInBatch: boolean;
  OnAfterLayout: Delegate<(revision: number) => void>;
  OnHostEvent: Delegate<(json: string) => void>;
  OnAnimationEvent: Delegate<(json: string) => void>;
  OnAnimationEventBatch?: Delegate<(json: string) => void>;
  OnAnimationEventBatchPacked?: Delegate<(payload: ArrayBuffer) => void>;
  OnNativeEvent: Delegate<(json: string) => void>; OnHostResponse: Delegate<(json: string) => void>;
  OnFrame: Delegate<(delta: number) => void>; OnLifecycle: Delegate<(action: string) => void>;
}
export const native: NativeBridge = require('puerts').argv.getByName('bridge');
export function findService<T extends object>(name: string): T | undefined {
  return require('puerts').argv.getByName(name) as T | undefined;
}
export function getService<T extends object>(name: string): T {
  const service = findService<T>(name);
  if (!service) throw new Error(`Unreal service is not registered: ${name}`);
  return service;
}
export function check(result: boolean | number): void {
  if (!result) throw new Error(native.LastError || 'RmlUi native operation failed');
}
export function report(error: unknown): void { native.ReportError(error instanceof Error ? error.stack || error.message : String(error)); }
