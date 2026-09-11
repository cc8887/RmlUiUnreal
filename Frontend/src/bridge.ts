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
  Listen(node: number, type: string, listener: number, capture: boolean): boolean; Unlisten(listener: number): void;
  SetEventResult(result: number): void; SaveState(json: string): void; ReportReady(): void;
  ReportError(message: string): void; ReportDebugState(json: string): void;
  RequestHost(id: number, method: string, json: string): void;
  ScrollNode(node: number, top: number): boolean; ScrollRemaining(node: number): number; FocusNode(node: number): boolean;
  OnHostEvent: Delegate<(json: string) => void>;
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
