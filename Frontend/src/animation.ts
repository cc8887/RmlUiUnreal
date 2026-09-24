import { native, report } from './bridge';

export type RmlAnimationRoute = 'native' | 'js_batched';
export type RmlAnimationState = 'running' | 'paused' | 'finished' | 'cancelled' | 'replaced';
export type RmlAnimationCompletionReason = 'completed' | 'cancelled' | 'replaced';
export type RmlAnimationDirection = 'normal' | 'reverse' | 'alternate' | 'alternate-reverse';
export type RmlAnimationFillMode = 'none' | 'forwards' | 'backwards' | 'both';
export type RmlAnimationCostClass = 'visual' | 'visual-discrete' | 'paint' | 'layout-position' | 'layout-size';

export interface RmlAnimationKeyframe {
  offset: number;
  value: number | string;
  easing?: string;
}

export interface RmlAnimationOptions {
  duration: number;
  delay?: number;
  iterations?: number;
  playbackRate?: number;
  direction?: RmlAnimationDirection;
  fill?: RmlAnimationFillMode;
  composite?: 'replace' | 'layered-replace';
  compositionOrder?: number;
  fallback?: 'js-batched' | 'reject';
}

export interface RmlAnimationCompletion {
  handle: string;
  route: RmlAnimationRoute;
  reason: RmlAnimationCompletionReason;
}

export interface RmlAnimation {
  readonly handle: string;
  readonly route: RmlAnimationRoute;
  readonly state: RmlAnimationState;
  readonly costClass?: RmlAnimationCostClass;
  readonly finished: Promise<RmlAnimationCompletion>;
  pause(): void;
  play(): void;
  seek(localTimeSeconds: number): void;
  setPlaybackRate(rate: number): void;
  cancel(): void;
  status(): RmlAnimationState;
}

interface NativeResult {
  accepted: boolean;
  route: 'native' | 'rejected';
  handle: string;
  state: string;
  error?: string;
}

interface NativeBatchResult {
  accepted: boolean;
  route: 'native' | 'rejected';
  handles: string[];
  state: string;
  error?: string;
  failedIndex?: number;
}

interface NativePlanBatchResult {
  accepted: boolean;
  handles: string[];
  allocatedBytes?: number[];
  error?: string;
  failedIndex?: number;
  released?: number;
}

export interface RmlAnimationRequest {
  node: number;
  property: string;
  keyframes: RmlAnimationKeyframe[];
  options: RmlAnimationOptions;
}

interface PropertyBatchResult { accepted: boolean; applied: number; error?: string; failedIndices?: number[] }
interface FallbackFrame {
  offset: number;
  values: number[];
  signature: string;
  serialize: (values: number[]) => string;
  easing: (value: number) => number;
}

let fallbackSequence = 0;
let currentClockSeconds = 0;
let frameRequest = 0;
let disposed = false;
const nativeAnimations = new Map<number, Map<number, AnimationController>>();
const fallbackAnimations = new Map<string, JsAnimationController>();
const animationsByTarget = new Map<string, Set<AnimationController>>();
let nativeCompletionBatchDispatches = 0;
let nativeCompletionEvents = 0;
let compiledPlanRegistrations = 0;
let compiledPlanCacheHits = 0;
let compiledStartBatches = 0;
let compiledPlanCacheBytes = 0;
let compiledPlanEvictions = 0;
let compiledPlanBudgetPressure = 0;
let compiledPlanClock = 0;
interface CompiledPlanCacheEntry {
  handle: string;
  allocatedBytes: number;
  activeBindings: number;
  lastUsed: number;
  costClass: RmlAnimationCostClass;
}
const compiledPlanCache = new Map<string, CompiledPlanCacheEntry>();

function nativeHandleWords(handle: string): { low: number; high: number } {
  const value = BigInt(handle);
  return { low: Number(value & 0xffffffffn), high: Number(value >> 32n) };
}

function findNativeAnimation(low: number, high: number): AnimationController | undefined {
  return nativeAnimations.get(high)?.get(low);
}

function addNativeAnimation(animation: AnimationController): void {
  const { low, high } = animation.nativeHandleWords!;
  let lowWords = nativeAnimations.get(high);
  if (!lowWords) {
    lowWords = new Map();
    nativeAnimations.set(high, lowWords);
  }
  lowWords.set(low, animation);
}

function removeNativeAnimation(animation: AnimationController): void {
  if (!animation.nativeHandleWords) return;
  const { low, high } = animation.nativeHandleWords;
  const lowWords = nativeAnimations.get(high);
  lowWords?.delete(low);
  if (lowWords?.size === 0) nativeAnimations.delete(high);
}

function isTerminalAnimationState(state: RmlAnimationState): boolean {
  return state === 'finished' || state === 'cancelled' || state === 'replaced';
}

function isStaleNativeHandleError(error: unknown): boolean {
  return error instanceof Error && error.message === 'stale_handle';
}

abstract class AnimationController implements RmlAnimation {
  protected currentState: RmlAnimationState = 'running';
  readonly finished: Promise<RmlAnimationCompletion>;
  readonly nativeHandleWords?: { low: number; high: number };
  private resolveFinished!: (result: RmlAnimationCompletion) => void;

  constructor(
    readonly handle: string,
    readonly route: RmlAnimationRoute,
    protected readonly targetKey: string,
    private readonly compiledPlanKey?: string,
    readonly costClass?: RmlAnimationCostClass,
  ) {
    if (route === 'native') this.nativeHandleWords = nativeHandleWords(handle);
    this.finished = new Promise(resolve => { this.resolveFinished = resolve; });
  }

  get state(): RmlAnimationState { return this.currentState; }
  abstract pause(): void;
  abstract play(): void;
  abstract seek(localTimeSeconds: number): void;
  abstract setPlaybackRate(rate: number): void;
  abstract cancel(): void;
  status(): RmlAnimationState { return this.currentState; }

  settle(reason: RmlAnimationCompletionReason): void {
    if (isTerminalAnimationState(this.currentState)) return;
    this.currentState = reason === 'completed' ? 'finished' : reason;
    const targetAnimations = animationsByTarget.get(this.targetKey);
    targetAnimations?.delete(this);
    if (targetAnimations?.size === 0) animationsByTarget.delete(this.targetKey);
    if (this.route === 'native') removeNativeAnimation(this);
    if (this.compiledPlanKey) releaseCompiledPlanBinding(this.compiledPlanKey);
    fallbackAnimations.delete(this.handle);
    this.resolveFinished({ handle: this.handle, route: this.route, reason });
  }
}

class NativeAnimationController extends AnimationController {
  private control(command: string, value = 0): NativeResult {
    const result = JSON.parse(native.ControlAnimation(this.handle, command, value)) as NativeResult;
    if (!result.accepted) throw new Error(result.error || `Native animation ${command} failed`);
    if (result.state === 'paused' || result.state === 'running') this.currentState = result.state;
    return result;
  }
  pause(): void { this.control('pause'); }
  play(): void { this.control('resume'); }
  seek(value: number): void { requireFiniteNonNegative(value, 'seek'); this.control('seek', value); }
  setPlaybackRate(value: number): void { requirePositive(value, 'playbackRate'); this.control('setplaybackrate', value); }
  cancel(): void {
    if (isTerminalAnimationState(this.currentState)) return;
    try {
      this.control('cancel');
    } catch (error) {
      // The native track can retire before its completion event reaches JS.
      // Treat that race as an idempotent cancellation; other control errors remain visible.
      if (!isStaleNativeHandleError(error)) throw error;
      this.settle('cancelled');
    }
  }
  status(): RmlAnimationState {
    if (isTerminalAnimationState(this.currentState)) return this.currentState;
    return this.control('status').state as RmlAnimationState;
  }
}

class JsAnimationController extends AnimationController {
  private originGlobal: number | undefined;
  private originLocal: number;
  private rate: number;
  private dirty = true;

  constructor(
    handle: string,
    targetKey: string,
    readonly node: number,
    readonly property: string,
    readonly frames: FallbackFrame[],
    readonly duration: number,
    readonly iterations: number,
    readonly direction: RmlAnimationDirection,
    readonly fill: RmlAnimationFillMode,
    readonly underlyingValue: string,
    delay: number,
    playbackRate: number,
  ) {
    super(handle, 'js_batched', targetKey, undefined, getAnimationPropertyCostClass(property));
    this.originLocal = -delay;
    this.rate = playbackRate;
  }

  private localTime(now: number): number {
    return this.originLocal + (this.originGlobal === undefined ? 0 : now - this.originGlobal) * this.rate;
  }
  private rebase(localTime = this.localTime(currentClockSeconds)): void {
    this.originLocal = localTime;
    this.originGlobal = currentClockSeconds;
  }
  pause(): void {
    if (this.currentState !== 'running') return;
    this.rebase(); this.currentState = 'paused';
  }
  play(): void {
    if (this.currentState !== 'paused') return;
    this.rebase(); this.currentState = 'running'; requestFallbackFrame();
  }
  seek(value: number): void {
    requireFiniteNonNegative(value, 'seek');
    this.rebase(Math.min(value, this.duration * this.iterations));
    this.dirty = true; requestFallbackFrame();
  }
  setPlaybackRate(value: number): void {
    requirePositive(value, 'playbackRate'); this.rebase(); this.rate = value;
    if (this.currentState === 'running') requestFallbackFrame();
  }
  private restore(reason: 'cancelled' | 'replaced'): void {
    try {
      const result = JSON.parse(native.ApplyNodePropertyBatch(JSON.stringify([
        { node: this.node, property: this.property, value: this.underlyingValue },
      ]))) as PropertyBatchResult;
      if (!result.accepted || result.applied !== 1)
        throw new Error(result.error || 'Animation underlying value restore failed');
    } catch (error) { report(error); }
    this.settle(reason);
  }
  cancel(): void { this.restore('cancelled'); }
  replace(): void { this.restore('replaced'); }
  needsSample(): boolean { return this.currentState === 'running' || this.dirty; }

  sample(now: number): { value: string; complete: boolean; contributes: boolean } {
    if (this.originGlobal === undefined) this.originGlobal = now;
    this.dirty = false;
    const local = this.localTime(now);
    const timing = iterationProgress(local, this.duration, this.iterations, this.direction);
    const contributes = local >= 0 || this.fill === 'backwards' || this.fill === 'both';
    if (timing.complete && this.fill !== 'forwards' && this.fill !== 'both')
      return { value: this.underlyingValue, complete: true, contributes: true };
    let segment = 0;
    while (segment + 1 < this.frames.length - 1 && timing.progress >= this.frames[segment + 1].offset) ++segment;
    const from = this.frames[segment], to = this.frames[segment + 1];
    const span = to.offset - from.offset;
    const alpha = from.easing(Math.max(0, Math.min(1, (timing.progress - from.offset) / span)));
    const values = from.values.map((value, index) => value + (to.values[index] - value) * alpha);
    return { value: from.serialize(values), complete: timing.complete, contributes };
  }
}

function requireFiniteNonNegative(value: number, name: string): void {
  if (!Number.isFinite(value) || value < 0) throw new Error(`${name} must be finite and non-negative`);
}
function requirePositive(value: number, name: string): void {
  if (!Number.isFinite(value) || value <= 0) throw new Error(`${name} must be finite and positive`);
}
function targetKey(node: number, property: string): string { return `${node}:${property.trim().toLowerCase()}`; }

function cubicCoordinate(t: number, p1: number, p2: number): number {
  const u = 1 - t;
  return 3 * u * u * t * p1 + 3 * u * t * t * p2 + t * t * t;
}

type StepPosition = 0 | 1 | 2 | 3;

function parseStepEasing(source: string): [number, StepPosition] | undefined {
  if (source === 'step-start') return [1, 1];
  if (source === 'step-end') return [1, 0];
  const match = source.match(/^steps\(\s*([1-9]\d*)\s*(?:,\s*(start|end|jump-start|jump-end|jump-none|jump-both)\s*)?\)$/);
  if (!match) return undefined;
  const count = Number(match[1]);
  if (!Number.isSafeInteger(count) || count > 65535) return undefined;
  const positions: Record<string, StepPosition> = {
    start: 1, end: 0, 'jump-start': 1, 'jump-end': 0, 'jump-none': 2, 'jump-both': 3,
  };
  const position = positions[match[2] || 'end'];
  return position === 2 && count < 2 ? undefined : [count, position];
}

function stepProgress(value: number, count: number, position: StepPosition): number {
  let current = Math.floor(Math.max(0, Math.min(1, value)) * count);
  let jumps = count;
  if (position === 1 || position === 3) ++current;
  if (position === 2) --jumps;
  else if (position === 3) ++jumps;
  return Math.max(0, Math.min(jumps, current)) / jumps;
}

function easing(source = 'linear'): (value: number) => number {
  const text = source.trim().toLowerCase();
  const presets: Record<string, [number, number, number, number]> = {
    ease: [0.25, 0.1, 0.25, 1], 'ease-in': [0.42, 0, 1, 1],
    'ease-out': [0, 0, 0.58, 1], 'ease-in-out': [0.42, 0, 0.58, 1],
  };
  if (!text || text === 'linear') return value => value;
  const steps = parseStepEasing(text);
  if (steps) return value => stepProgress(value, steps[0], steps[1]);
  const powerMatch = text.match(/^rml-power\((in|out|inout),\s*([0-9]+(?:\.[0-9]+)?)\)$/);
  if (powerMatch) {
    const mode = powerMatch[1];
    const power = Number(powerMatch[2]);
    if (!Number.isFinite(power) || power <= 0 || power > 16) throw new Error(`Unsupported fallback easing: ${source}`);
    if (mode === 'in') return value => Math.pow(value, power);
    if (mode === 'out') return value => 1 - Math.pow(1 - value, power);
    return value => value < 0.5
      ? Math.pow(value * 2, power) / 2
      : 1 - Math.pow((1 - value) * 2, power) / 2;
  }
  const match = text.match(/^cubic-bezier\(\s*([^,]+),\s*([^,]+),\s*([^,]+),\s*([^\)]+)\)$/);
  const points = presets[text] || (match ? match.slice(1).map(Number) as [number, number, number, number] : undefined);
  if (!points || points.some(value => !Number.isFinite(value)) || points[0] < 0 || points[0] > 1 || points[2] < 0 || points[2] > 1)
    throw new Error(`Unsupported fallback easing: ${source}`);
  return x => {
    let low = 0, high = 1;
    for (let index = 0; index < 16; ++index) {
      const mid = (low + high) * 0.5;
      if (cubicCoordinate(mid, points[0], points[2]) < x) low = mid; else high = mid;
    }
    return cubicCoordinate((low + high) * 0.5, points[1], points[3]);
  };
}

function formatNumber(value: number): string { return String(Number(value.toFixed(6))); }

function fallbackFrames(input: RmlAnimationKeyframe[]): FallbackFrame[] {
  if (input.length < 2 || input.length > 4096) throw new Error('Fallback requires 2 to 4096 keyframes');
  const scalarPattern = /^([+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:e[+-]?\d+)?)(.*)$/i;
  const values = input.map(frame => String(frame.value).trim().toLowerCase());
  const scalarMatches = values.map(value => value.match(scalarPattern));
  let signature = '';
  let parse: (value: string) => { values: number[]; signature: string; serialize: (values: number[]) => string };
  if (scalarMatches.every(Boolean)) {
    parse = value => {
      const match = value.match(scalarPattern)!;
      const unit = match[2].trim();
      return { values: [Number(match[1])], signature: `scalar:${unit}`, serialize: result => `${formatNumber(result[0])}${unit}` };
    };
  } else {
    const primitives = values.filter(value => value !== 'none').map(value => value.match(/^(scale|rotate|translate)\((.*)\)$/)?.[1]);
    const primitive = primitives[0];
    if (!primitive || primitives.some(value => value !== primitive)) throw new Error('Fallback supports finite scalars or one transform primitive only');
    parse = value => {
      if (value === 'none') {
        if (primitive === 'scale') return { values: [1], signature: 'transform:scale', serialize: result => `scale(${formatNumber(result[0])})` };
        if (primitive === 'rotate') return { values: [0], signature: 'transform:rotate:deg', serialize: result => `rotate(${formatNumber(result[0])}deg)` };
        return { values: [0, 0], signature: 'transform:translate:px:px', serialize: result => `translate(${formatNumber(result[0])}px,${formatNumber(result[1])}px)` };
      }
      const match = value.match(/^(scale|rotate|translate)\((.*)\)$/)!;
      const parts = match[2].split(',').map(part => part.trim());
      if (primitive === 'scale') {
        if (parts.length !== 1 || !scalarPattern.test(parts[0])) throw new Error('Fallback scale requires one finite value');
        const number = parts[0].match(scalarPattern)!;
        if (number[2].trim()) throw new Error('Fallback scale must be unitless');
        return { values: [Number(number[1])], signature: 'transform:scale', serialize: result => `scale(${formatNumber(result[0])})` };
      }
      if (primitive === 'rotate') {
        if (parts.length !== 1 || !scalarPattern.test(parts[0])) throw new Error('Fallback rotate requires one finite value');
        const number = parts[0].match(scalarPattern)!;
        const unit = number[2].trim() || 'deg';
        if (!['deg', 'rad', 'turn'].includes(unit)) throw new Error('Fallback rotate unit is unsupported');
        return { values: [Number(number[1])], signature: `transform:rotate:${unit}`, serialize: result => `rotate(${formatNumber(result[0])}${unit})` };
      }
      if (parts.length !== 2) throw new Error('Fallback translate requires two values');
      const numbers = parts.map(part => part.match(scalarPattern));
      if (numbers.some(match => !match)) throw new Error('Fallback translate requires finite values');
      const units = numbers.map(match => match![2].trim() || 'px');
      if (units.some(unit => unit !== 'px')) throw new Error('Fallback translate currently requires px values');
      return {
        values: numbers.map(match => Number(match![1])), signature: 'transform:translate:px:px',
        serialize: result => `translate(${formatNumber(result[0])}px,${formatNumber(result[1])}px)`,
      };
    };
  }
  let previous = -1;
  return input.map((frame, index) => {
    if (!Number.isFinite(frame.offset) || frame.offset <= previous ||
        (index === 0 && frame.offset !== 0) || (index === input.length - 1 && frame.offset !== 1))
      throw new Error('Fallback keyframe offsets must increase strictly from 0 to 1');
    previous = frame.offset;
    const parsed = parse(values[index]);
    if (parsed.values.some(value => !Number.isFinite(value))) throw new Error('Fallback values must be finite');
    if (!signature) signature = parsed.signature;
    if (parsed.signature !== signature)
      throw new Error('Fallback keyframes must use one value shape and unit');
    return { offset: frame.offset, ...parsed, easing: easing(frame.easing) };
  });
}

function iterationProgress(local: number, duration: number, iterations: number, direction: RmlAnimationDirection) {
  const reverseAt = (iteration: number) => direction === 'reverse' ||
    (direction === 'alternate' && (iteration & 1) === 1) ||
    (direction === 'alternate-reverse' && (iteration & 1) === 0);
  if (local <= 0) return { progress: reverseAt(0) ? 1 : 0, complete: false };
  const total = duration * iterations;
  const complete = local >= total;
  const iteration = complete ? iterations - 1 : Math.min(iterations - 1, Math.floor(local / duration));
  let progress = complete ? 1 : (local % duration) / duration;
  if (reverseAt(iteration)) progress = 1 - progress;
  return { progress, complete };
}

function requestFallbackFrame(): void {
  if (!disposed && !frameRequest && [...fallbackAnimations.values()].some(animation => animation.needsSample()))
    frameRequest = requestAnimationFrame(flushFallbackFrame);
}

function flushFallbackFrame(timeMilliseconds: number): void {
  frameRequest = 0;
  currentClockSeconds = timeMilliseconds / 1000;
  const sampled: JsAnimationController[] = [];
  const completed: JsAnimationController[] = [];
  const updates: Array<{ node: number; property: string; value: string }> = [];
  for (const animation of fallbackAnimations.values()) {
    if (!animation.needsSample()) continue;
    const sample = animation.sample(currentClockSeconds);
    if (sample.contributes) {
      sampled.push(animation);
      updates.push({ node: animation.node, property: animation.property, value: sample.value });
    }
    if (sample.complete && animation.state === 'running') completed.push(animation);
  }
  if (updates.length) {
    try {
      let pendingAnimations = sampled;
      let pendingUpdates = updates;
      while (pendingUpdates.length) {
        const result = JSON.parse(native.ApplyNodePropertyBatch(JSON.stringify(pendingUpdates))) as PropertyBatchResult;
        if (result.accepted && result.applied === pendingUpdates.length) break;
        const failed = result.error === 'stale_property_target' && Array.isArray(result.failedIndices)
          ? [...new Set(result.failedIndices)].sort((left, right) => left - right)
          : [];
        if (!failed.length || failed.some(index => !Number.isInteger(index) || index < 0 || index >= pendingUpdates.length))
          throw new Error(result.error || 'Incomplete property batch');
        const stale = new Set(failed);
        pendingAnimations.forEach((animation, index) => { if (stale.has(index)) animation.settle('cancelled'); });
        pendingAnimations = pendingAnimations.filter((_, index) => !stale.has(index));
        pendingUpdates = pendingUpdates.filter((_, index) => !stale.has(index));
      }
      for (const animation of completed) animation.settle('completed');
    } catch (error) {
      report(error);
      for (const animation of sampled) animation.settle('cancelled');
    }
  }
  requestFallbackFrame();
}

function onNativeAnimationEvents(json: string): void {
  try {
    const parsed = JSON.parse(json) as
      { handle: string; reason: RmlAnimationCompletionReason } |
      { handle: string; reason: RmlAnimationCompletionReason }[];
    const events = Array.isArray(parsed) ? parsed : [parsed];
    ++nativeCompletionBatchDispatches;
    nativeCompletionEvents += events.length;
    for (const event of events) {
      const { low, high } = nativeHandleWords(event.handle);
      findNativeAnimation(low, high)?.settle(event.reason);
    }
    trimCompiledPlanCache();
  } catch (error) { report(error); }
}

function onNativeAnimationEventsPacked(payload: ArrayBuffer): void {
  try {
    const magic = 0x31454152;
    const headerSize = 8;
    const stride = 12;
    const view = new DataView(payload);
    if (payload.byteLength < headerSize || view.getUint32(0, true) !== magic)
      throw new Error('Invalid packed animation completion batch');
    const count = view.getUint32(4, true);
    if (payload.byteLength !== headerSize + count * stride)
      throw new Error('Invalid packed animation completion batch length');
    const reasonNames: RmlAnimationCompletionReason[] = ['completed', 'cancelled', 'replaced'];
    ++nativeCompletionBatchDispatches;
    nativeCompletionEvents += count;
    for (let offset = headerSize; offset < payload.byteLength; offset += stride) {
      const reason = reasonNames[view.getUint8(offset + 8)];
      if (!reason) throw new Error('Invalid packed animation completion reason');
      findNativeAnimation(
        view.getUint32(offset, true), view.getUint32(offset + 4, true))?.settle(reason);
    }
    trimCompiledPlanCache();
  } catch (error) { report(error); }
}

const nativeAnimationEventPackedDelegate = native.bUsePackedAnimationEvents === false
  ? undefined : native.OnAnimationEventBatchPacked;
const nativeAnimationEventDelegate = native.OnAnimationEventBatch ?? native.OnAnimationEvent;

export function getAnimationCompletionDebugState(): {
  transport: 'packed' | 'json' | 'legacy';
  batchDispatches: number;
  events: number;
} {
  return {
    transport: nativeAnimationEventPackedDelegate
      ? 'packed' : native.OnAnimationEventBatch ? 'json' : 'legacy',
    batchDispatches: nativeCompletionBatchDispatches,
    events: nativeCompletionEvents,
  };
}

function replaceForFallback(key: string): void {
  const existing = [...(animationsByTarget.get(key) ?? [])];
  for (const animation of existing) {
    if (animation instanceof JsAnimationController) animation.replace();
    else animation.settle('replaced');
    if (animation.route === 'native') {
      try { native.ControlAnimation(animation.handle, 'cancel', 0); } catch (error) { report(error); }
    }
  }
}

function trackTargetAnimation(key: string, animation: AnimationController): void {
  let targetAnimations = animationsByTarget.get(key);
  if (!targetAnimations) {
    targetAnimations = new Set();
    animationsByTarget.set(key, targetAnimations);
  }
  targetAnimations.add(animation);
}

interface CompiledKeyframe {
  offset: number;
  values: number[];
  easing: [number, number, number, number, number];
}

interface CompiledPlan {
  key: string;
  property: number;
  costClass: RmlAnimationCostClass;
  direction: number;
  fill: number;
  iterations: number;
  duration: number;
  delay: number;
  playbackRate: number;
  keyframes: CompiledKeyframe[];
}

interface NativePropertyDescriptor {
  id: number;
  costClass: RmlAnimationCostClass;
}

const NATIVE_PROPERTY_DESCRIPTORS: Readonly<Record<string, NativePropertyDescriptor>> = {
  opacity: { id: 1, costClass: 'visual' },
  transform: { id: 2, costClass: 'visual' },
  left: { id: 3, costClass: 'layout-position' },
  top: { id: 4, costClass: 'layout-position' },
  right: { id: 5, costClass: 'layout-position' },
  bottom: { id: 6, costClass: 'layout-position' },
  width: { id: 7, costClass: 'layout-size' },
  height: { id: 8, costClass: 'layout-size' },
  visibility: { id: 9, costClass: 'visual-discrete' },
  color: { id: 10, costClass: 'paint' },
  'background-color': { id: 11, costClass: 'paint' },
  'border-color': { id: 12, costClass: 'paint' },
  'image-color': { id: 13, costClass: 'paint' },
};

export function getAnimationPropertyCostClass(property: string): RmlAnimationCostClass | undefined {
  return NATIVE_PROPERTY_DESCRIPTORS[property.trim().toLowerCase()]?.costClass;
}

function compiledEasing(source = 'linear'): [number, number, number, number, number] | undefined {
  const text = source.trim().toLowerCase();
  const presets: Record<string, [number, number, number, number]> = {
    ease: [0.25, 0.1, 0.25, 1], 'ease-in': [0.42, 0, 1, 1],
    'ease-out': [0, 0, 0.58, 1], 'ease-in-out': [0.42, 0, 0.58, 1],
  };
  if (!text || text === 'linear') return [0, 0, 0, 1, 1];
  const steps = parseStepEasing(text);
  if (steps) return [2, steps[0], steps[1], 0, 0];
  const match = text.match(/^cubic-bezier\(\s*([^,]+),\s*([^,]+),\s*([^,]+),\s*([^\)]+)\)$/);
  const points = presets[text] || (match ? match.slice(1).map(Number) as [number, number, number, number] : undefined);
  if (!points || points.some(value => !Number.isFinite(value)) ||
      points[0] < 0 || points[0] > 1 || points[2] < 0 || points[2] > 1) return undefined;
  return [1, ...points.map(Math.fround)] as [number, number, number, number, number];
}

function compiledColor(value: number | string): number[] | undefined {
  const text = String(value).trim().toLowerCase();
  if (text === 'transparent') return [0, 0, 0, 0];
  const hex = text.match(/^#([0-9a-f]{3,4}|[0-9a-f]{6}|[0-9a-f]{8})$/i)?.[1];
  if (hex) {
    const expanded = hex.length <= 4 ? [...hex].map(char => char + char).join('') : hex;
    const channels = [0, 2, 4, 6].map((offset, index) => index === 3 && expanded.length === 6
      ? 255 : Number.parseInt(expanded.slice(offset, offset + 2), 16));
    return channels.map(channel => Math.fround(channel / 255));
  }
  const rgb = text.match(/^rgba?\(\s*([^,]+),\s*([^,]+),\s*([^,\)]+)(?:,\s*([^\)]+))?\)$/);
  if (!rgb) return undefined;
  const channels = [Number(rgb[1]) / 255, Number(rgb[2]) / 255, Number(rgb[3]) / 255,
    rgb[4] === undefined ? 1 : Number(rgb[4])];
  return channels.every(channel => Number.isFinite(channel) && channel >= 0 && channel <= 1)
    ? channels.map(Math.fround) : undefined;
}

function compiledTransform(value: number | string): { values: number[]; primitive: number } | undefined {
  const text = String(value).trim().toLowerCase();
  if (text === 'none') return { values: [0, 0, 1, 1, 0, 0, 0], primitive: 0 };
  const finite = (source: string, suffix: string): number | undefined => {
    let numberText = source;
    if (suffix) {
      if (!numberText.endsWith(suffix)) return undefined;
      numberText = numberText.slice(0, -suffix.length).trim();
    }
    const parsed = Number(numberText);
    return numberText && Number.isFinite(parsed) ? Math.fround(parsed) : undefined;
  };
  const values = [0, 0, 1, 1, 0, 0, 0];
  let primitive = 0, cursor = 0;
  const pattern = /\s*(scale|translate|rotate|skewx|skewy|skew|matrix)\(([^\(\)]*)\)/gy;
  while (cursor < text.length) {
    pattern.lastIndex = cursor;
    const match = pattern.exec(text);
    if (!match || match.index !== cursor) return undefined;
    const parts = match[2].split(',').map(part => part.trim());
    const bit = match[1] === 'translate' ? 1 : match[1] === 'scale' ? 2 : match[1] === 'rotate' ? 4 :
      match[1] === 'skewx' ? 8 : match[1] === 'skewy' ? 16 : match[1] === 'skew' ? 24 : 31;
    if (primitive & bit) return undefined;
    if (bit === 1 && parts.length === 2) {
      const x = finite(parts[0], 'px'), y = finite(parts[1], 'px');
      if (x === undefined || y === undefined) return undefined;
      values[0] = x; values[1] = y;
    } else if (bit === 2 && (parts.length === 1 || parts.length === 2)) {
      const x = finite(parts[0], ''), y = parts.length === 2 ? finite(parts[1], '') : x;
      if (x === undefined || y === undefined) return undefined;
      values[2] = x; values[3] = y;
    } else if (bit === 4 && parts.length === 1) {
      const angle = finite(parts[0], 'deg');
      if (angle === undefined) return undefined;
      values[4] = angle;
    } else if ((bit === 8 || bit === 16 || bit === 24) && (parts.length === 1 || (bit === 24 && parts.length === 2))) {
      const x = finite(parts[0], 'deg'), y = parts.length === 2 ? finite(parts[1], 'deg') : 0;
      if (x === undefined || y === undefined) return undefined;
      if (bit === 16) values[6] = x; else { values[5] = x; values[6] = y; }
    } else if (bit === 31 && parts.length === 6) {
      const matrix = parts.map(part => finite(part, ''));
      if (matrix.some(component => component === undefined)) return undefined;
      const [a, b, c, d, tx, ty] = matrix as number[];
      const scaleX = Math.hypot(a, b);
      if (scaleX <= Number.EPSILON) return undefined;
      values[0] = tx; values[1] = ty; values[2] = scaleX; values[3] = (a * d - b * c) / scaleX;
      values[4] = Math.atan2(b, a) * 180 / Math.PI;
      values[5] = Math.atan((a * c + b * d) / (scaleX * scaleX)) * 180 / Math.PI;
    } else return undefined;
    primitive |= bit;
    cursor = pattern.lastIndex;
  }
  return primitive ? { values, primitive } : undefined;
}

function compileNativePlan(request: {
  property: string;
  keyframes: RmlAnimationKeyframe[];
  nativeOptions: {
    duration: number; delay: number; iterations: number; playbackRate: number;
    direction: RmlAnimationDirection; composite: 'replace' | 'layered-replace';
    fill: RmlAnimationFillMode;
    compositionOrder?: number;
  };
}): CompiledPlan | undefined {
  if (request.keyframes.length < 2 || request.keyframes.length > 4096) return undefined;
  const propertyText = request.property.trim().toLowerCase();
  const descriptor = NATIVE_PROPERTY_DESCRIPTORS[propertyText];
  if (!descriptor) return undefined;
  const property = descriptor.id;
  const direction = ['normal', 'reverse', 'alternate', 'alternate-reverse'].indexOf(request.nativeOptions.direction);
  if (direction < 0) return undefined;
  const fill = ['none', 'forwards', 'backwards', 'both'].indexOf(request.nativeOptions.fill);
  if (fill < 0) return undefined;
  let commonPrimitive = 0;
  const keyframes: CompiledKeyframe[] = [];
  for (const frame of request.keyframes) {
    if (!Number.isFinite(frame.offset)) return undefined;
    const easingValues = compiledEasing(frame.easing);
    if (!easingValues) return undefined;
    if (property !== 2 && property < 10) {
      let value: number;
      if (property === 1) {
        value = Number(frame.value);
        if (!Number.isFinite(value) || value < 0 || value > 1) return undefined;
      } else if (property === 9) {
        const visibility = String(frame.value).trim().toLowerCase();
        if (visibility !== 'visible' && visibility !== 'hidden') return undefined;
        value = visibility === 'visible' ? 1 : 0;
      } else {
        const match = String(frame.value).trim().match(/^([+-]?(?:\d+(?:\.\d*)?|\.\d+))px$/i);
        if (!match) return undefined;
        value = Number(match[1]);
        if (!Number.isFinite(value) || ((property === 7 || property === 8) && value < 0)) return undefined;
      }
      keyframes.push({ offset: Math.fround(frame.offset), values: [Math.fround(value)], easing: easingValues });
    } else if (property === 2) {
      const parsed = compiledTransform(frame.value);
      if (!parsed) return undefined;
      if (parsed.primitive !== 0) {
        if (commonPrimitive !== 0 && commonPrimitive !== parsed.primitive) return undefined;
        commonPrimitive = parsed.primitive;
      }
      keyframes.push({ offset: Math.fround(frame.offset), values: parsed.values, easing: easingValues });
    } else {
      const parsed = compiledColor(frame.value);
      if (!parsed) return undefined;
      keyframes.push({ offset: Math.fround(frame.offset), values: parsed, easing: easingValues });
    }
  }
  const plan: CompiledPlan = {
    property, costClass: descriptor.costClass, direction, fill, iterations: request.nativeOptions.iterations,
    duration: request.nativeOptions.duration, delay: request.nativeOptions.delay,
    playbackRate: request.nativeOptions.playbackRate, keyframes, key: '',
  };
  plan.key = JSON.stringify([property, direction, fill, plan.iterations, plan.duration, plan.delay,
    plan.playbackRate, keyframes.map(frame => [frame.offset, ...frame.values, ...frame.easing])]);
  return plan;
}

function encodePlanBatch(plans: CompiledPlan[]): ArrayBuffer {
  const byteLength = 12 + plans.reduce((sum, plan) => sum + 40 +
    plan.keyframes.length * (plan.property === 2 ? 52 : plan.property >= 10 ? 40 : 28), 0);
  const buffer = new ArrayBuffer(byteLength);
  const view = new DataView(buffer);
  view.setUint32(0, 0x31504152, true); view.setUint16(4, 3, true); view.setUint32(8, plans.length, true);
  let offset = 12;
  for (const plan of plans) {
    view.setUint8(offset, plan.property); view.setUint8(offset + 1, plan.direction);
    view.setUint16(offset + 2, plan.keyframes.length, true);
    view.setUint32(offset + 4, plan.iterations, true);
    view.setFloat64(offset + 8, plan.duration, true); view.setFloat64(offset + 16, plan.delay, true);
    view.setFloat64(offset + 24, plan.playbackRate, true);
    view.setUint8(offset + 32, plan.fill); offset += 40;
    for (const frame of plan.keyframes) {
      view.setFloat32(offset, frame.offset, true); offset += 4;
      for (const value of frame.values) { view.setFloat32(offset, value, true); offset += 4; }
      view.setUint8(offset, frame.easing[0]); offset += 4;
      for (let index = 1; index < frame.easing.length; ++index) {
        view.setFloat32(offset, frame.easing[index], true); offset += 4;
      }
    }
  }
  return buffer;
}

function compiledPlanEstimatedAllocatedBytes(plan: CompiledPlan): number {
  // Native definitions expand each segment easing to a 33-sample float LUT.
  return plan.keyframes.length * (plan.property === 2 ? 164 : plan.property >= 10 ? 152 : 140);
}

function compiledPlanCacheLimits(): { entries: number; bytes: number } {
  const entries = native.CompiledAnimationPlanCacheMaxEntries;
  const bytes = native.CompiledAnimationPlanCacheMaxBytes;
  return {
    entries: Number.isInteger(entries) && entries! > 0 ? entries! : 1024,
    bytes: Number.isFinite(bytes) && bytes! >= 1024 ? bytes! : 16 * 1024 * 1024,
  };
}

function releaseCompiledPlanEntries(
  entries: Array<[string, CompiledPlanCacheEntry]>,
  countEviction = true,
): void {
  if (!entries.length || !native.ReleaseAnimationPlansPacked) return;
  const buffer = new ArrayBuffer(12 + entries.length * 8);
  const view = new DataView(buffer);
  view.setUint32(0, 0x31524152, true); view.setUint16(4, 1, true); view.setUint32(8, entries.length, true);
  let offset = 12;
  for (const [, entry] of entries) {
    const words = nativeHandleWords(entry.handle);
    view.setUint32(offset, words.low, true); view.setUint32(offset + 4, words.high, true); offset += 8;
  }
  const result = JSON.parse(native.ReleaseAnimationPlansPacked(buffer)) as NativePlanBatchResult;
  if (!result.accepted || result.released !== entries.length)
    throw new Error(result.error || 'Native animation plans were not fully released');
  for (const [key, entry] of entries) {
    if (compiledPlanCache.get(key) !== entry) continue;
    compiledPlanCache.delete(key);
    compiledPlanCacheBytes -= entry.allocatedBytes;
    if (countEviction) ++compiledPlanEvictions;
  }
}

function trimCompiledPlanCache(
  protectedKeys: ReadonlySet<string> = new Set(),
  incomingEntries = 0,
  incomingBytes = 0,
): void {
  const limits = compiledPlanCacheLimits();
  let projectedEntries = compiledPlanCache.size + incomingEntries;
  let projectedBytes = compiledPlanCacheBytes + incomingBytes;
  if (projectedEntries <= limits.entries && projectedBytes <= limits.bytes) return;
  const candidates = [...compiledPlanCache.entries()]
    .filter(([key, entry]) => entry.activeBindings === 0 && !protectedKeys.has(key))
    .sort((left, right) => left[1].lastUsed - right[1].lastUsed);
  const evicted: Array<[string, CompiledPlanCacheEntry]> = [];
  for (const candidate of candidates) {
    if (projectedEntries <= limits.entries && projectedBytes <= limits.bytes) break;
    evicted.push(candidate);
    --projectedEntries;
    projectedBytes -= candidate[1].allocatedBytes;
  }
  releaseCompiledPlanEntries(evicted);
  if (projectedEntries > limits.entries || projectedBytes > limits.bytes) ++compiledPlanBudgetPressure;
}

function releaseCompiledPlanBinding(key: string): void {
  const entry = compiledPlanCache.get(key);
  if (entry && entry.activeBindings > 0) --entry.activeBindings;
}

function encodeCompiledStarts(requests: Array<{
  node: number;
  nativeOptions: { composite: 'replace' | 'layered-replace'; compositionOrder?: number };
}>, handles: string[]): ArrayBuffer {
  const buffer = new ArrayBuffer(12 + requests.length * 24);
  const view = new DataView(buffer);
  view.setUint32(0, 0x31494152, true); view.setUint16(4, 1, true); view.setUint32(8, requests.length, true);
  let offset = 12;
  for (let index = 0; index < requests.length; ++index, offset += 24) {
    const words = nativeHandleWords(handles[index]);
    view.setUint32(offset, words.low, true); view.setUint32(offset + 4, words.high, true);
    view.setUint32(offset + 8, requests[index].node, true);
    view.setInt32(offset + 12, requests[index].nativeOptions.compositionOrder ?? 0, true);
    view.setUint8(offset + 16, requests[index].nativeOptions.composite === 'layered-replace' ? 1 : 0);
  }
  return buffer;
}

function releaseCompiledPlans(): void {
  if (!compiledPlanCache.size || !native.ReleaseAnimationPlansPacked) return;
  releaseCompiledPlanEntries([...compiledPlanCache.entries()], false);
}

export function getAnimationStartDebugState(): {
  transport: 'compiled-packed' | 'json'; registrations: number; cacheHits: number; batches: number;
  cachedPlans: number; cacheBytes: number; activeBindings: number; evictions: number; budgetPressure: number;
  nativeAllocatedBytes?: number;
  cachedPlansByCost: { visual: number; visualDiscrete: number; paint: number; layoutPosition: number; layoutSize: number };
  activeTracksByCost?: { activeTracks: number; visual: number; visualDiscrete: number; paint: number; layoutPosition: number; layoutSize: number };
} {
  const packed = native.bUseCompiledAnimationPlans !== false && !!native.RegisterAnimationPlansPacked &&
    !!native.StartCompiledAnimationBatchPacked && !!native.ReleaseAnimationPlansPacked;
  let nativeAllocatedBytes: number | undefined;
  let activeTracksByCost: { activeTracks: number; visual: number; visualDiscrete: number; paint: number; layoutPosition: number; layoutSize: number } | undefined;
  if (native.GetAnimationPlanCacheStats) {
    try { nativeAllocatedBytes = JSON.parse(native.GetAnimationPlanCacheStats()).allocatedBytes; }
    catch (error) { report(error); }
  }
  if (native.GetAnimationRuntimeStats) {
    try { activeTracksByCost = JSON.parse(native.GetAnimationRuntimeStats()); }
    catch (error) { report(error); }
  }
  const cachedPlansByCost = { visual: 0, visualDiscrete: 0, paint: 0, layoutPosition: 0, layoutSize: 0 };
  for (const entry of compiledPlanCache.values()) {
    if (entry.costClass === 'visual') ++cachedPlansByCost.visual;
    else if (entry.costClass === 'visual-discrete') ++cachedPlansByCost.visualDiscrete;
    else if (entry.costClass === 'paint') ++cachedPlansByCost.paint;
    else if (entry.costClass === 'layout-position') ++cachedPlansByCost.layoutPosition;
    else ++cachedPlansByCost.layoutSize;
  }
  return { transport: packed ? 'compiled-packed' : 'json', registrations: compiledPlanRegistrations,
    cacheHits: compiledPlanCacheHits, batches: compiledStartBatches, cachedPlans: compiledPlanCache.size,
    cacheBytes: compiledPlanCacheBytes,
    activeBindings: [...compiledPlanCache.values()].reduce((sum, entry) => sum + entry.activeBindings, 0),
    evictions: compiledPlanEvictions, budgetPressure: compiledPlanBudgetPressure, nativeAllocatedBytes,
    cachedPlansByCost, activeTracksByCost };
}

export function startAnimations(requests: RmlAnimationRequest[]): RmlAnimation[] {
  if (disposed) throw new Error('Animation runtime has been disposed');
  if (!Array.isArray(requests) || !requests.length) return [];
  const compositionByKey = new Map<string, Set<number> | undefined>();
  const normalized = requests.map(request => {
    const { node, property, keyframes, options } = request;
    if (!Number.isInteger(node) || node <= 0 || !native.IsNodeValid(node)) throw new Error('Animation target is invalid');
    requireFiniteNonNegative(options.duration, 'duration');
    const delay = options.delay ?? 0; requireFiniteNonNegative(delay, 'delay');
    const iterations = options.iterations ?? 1;
    if (!Number.isInteger(iterations) || iterations < 1) throw new Error('iterations must be a positive integer');
    const playbackRate = options.playbackRate ?? 1; requirePositive(playbackRate, 'playbackRate');
    const direction = options.direction ?? 'normal';
    if (!['normal', 'reverse', 'alternate', 'alternate-reverse'].includes(direction)) throw new Error('Unsupported direction');
    const fill: RmlAnimationFillMode = options.fill ?? 'both';
    if (!['none', 'forwards', 'backwards', 'both'].includes(fill)) throw new Error('Unsupported animation fill mode');
    const composite = options.composite ?? 'replace';
    if (composite !== 'replace' && composite !== 'layered-replace')
      throw new Error('Unsupported animation composite mode');
    const compositionOrder = options.compositionOrder;
    if (composite === 'layered-replace' && !Number.isInteger(compositionOrder))
      throw new Error('layered-replace requires an integer compositionOrder');
    if (composite === 'replace' && compositionOrder !== undefined)
      throw new Error('compositionOrder requires layered-replace');
    const key = targetKey(node, property);
    if (compositionByKey.has(key)) {
      const orders = compositionByKey.get(key);
      if (!orders || composite !== 'layered-replace' || orders.has(compositionOrder!))
        throw new Error('Animation batch contains duplicate target/property tracks or contribution orders');
      orders.add(compositionOrder!);
    } else {
      compositionByKey.set(key, composite === 'layered-replace'
        ? new Set([compositionOrder!]) : undefined);
    }
    return {
      node, property: property.trim(), keyframes, options, key,
      nativeOptions: { duration: options.duration, delay, iterations, playbackRate, direction,
        fill, composite,
        ...(compositionOrder === undefined ? {} : { compositionOrder }) },
    };
  });

  let result: NativeBatchResult;
  const useCompiledPlans = native.bUseCompiledAnimationPlans !== false &&
    !!native.RegisterAnimationPlansPacked && !!native.StartCompiledAnimationBatchPacked &&
    !!native.ReleaseAnimationPlansPacked;
  const compiledPlans = useCompiledPlans ? normalized.map(compileNativePlan) : [];
  let usedCompiledPlans: CompiledPlan[] | undefined;
  let compiledReservationsActive = false;
  if (useCompiledPlans && compiledPlans.every((plan): plan is CompiledPlan => !!plan)) {
    usedCompiledPlans = compiledPlans;
    const protectedKeys = new Set(compiledPlans.map(plan => plan.key));
    for (const key of protectedKeys) {
      const entry = compiledPlanCache.get(key);
      if (entry) entry.lastUsed = ++compiledPlanClock;
    }
    const missingByKey = new Map<string, CompiledPlan>();
    for (const plan of compiledPlans) {
      if (!compiledPlanCache.has(plan.key) && !missingByKey.has(plan.key)) missingByKey.set(plan.key, plan);
    }
    const missing = [...missingByKey.values()];
    if (missing.length) {
      trimCompiledPlanCache(protectedKeys, missing.length,
        missing.reduce((sum, plan) => sum + compiledPlanEstimatedAllocatedBytes(plan), 0));
      const registration = JSON.parse(native.RegisterAnimationPlansPacked!(encodePlanBatch(missing))) as NativePlanBatchResult;
      const validHandles = registration.accepted && Array.isArray(registration.handles) &&
        registration.handles.length === missing.length &&
        registration.handles.every(handle => typeof handle === 'string' && handle.length > 0) &&
        new Set(registration.handles).size === registration.handles.length;
      if (!validHandles) throw new Error(registration.error || 'Native animation plan registration failed');
      const validAllocatedBytes = registration.allocatedBytes === undefined ||
        (Array.isArray(registration.allocatedBytes) && registration.allocatedBytes.length === missing.length &&
          registration.allocatedBytes.every(value => Number.isFinite(value) && value >= 0));
      if (!validAllocatedBytes) throw new Error('Native animation plan registration returned invalid allocation sizes');
      missing.forEach((plan, index) => {
        const entry: CompiledPlanCacheEntry = {
          handle: registration.handles[index],
          allocatedBytes: registration.allocatedBytes?.[index] ?? compiledPlanEstimatedAllocatedBytes(plan),
          activeBindings: 0, lastUsed: ++compiledPlanClock,
          costClass: plan.costClass,
        };
        compiledPlanCache.set(plan.key, entry);
        compiledPlanCacheBytes += entry.allocatedBytes;
      });
      compiledPlanRegistrations += missing.length;
    }
    compiledPlanCacheHits += compiledPlans.length - missing.length;
    const planHandles = compiledPlans.map(plan => compiledPlanCache.get(plan.key)!.handle);
    for (const plan of compiledPlans) {
      const entry = compiledPlanCache.get(plan.key)!;
      ++entry.activeBindings;
      entry.lastUsed = ++compiledPlanClock;
    }
    compiledReservationsActive = true;
    try {
      result = JSON.parse(native.StartCompiledAnimationBatchPacked!(
        encodeCompiledStarts(normalized, planHandles))) as NativeBatchResult;
      ++compiledStartBatches;
    } catch (error) {
      for (const plan of compiledPlans) releaseCompiledPlanBinding(plan.key);
      compiledReservationsActive = false;
      throw error;
    }
    if (!result.accepted || result.route !== 'native') {
      for (const plan of compiledPlans) releaseCompiledPlanBinding(plan.key);
      compiledReservationsActive = false;
      usedCompiledPlans = undefined;
    }
  } else {
    result = JSON.parse(native.StartNodeKeyframeAnimationBatch(JSON.stringify(
      normalized.map(request => ({
        node: request.node,
        property: request.property,
        keyframes: request.keyframes,
        options: request.nativeOptions,
      })),
    ))) as NativeBatchResult;
  }
  if (result.accepted && result.route === 'native') {
    const validHandles = Array.isArray(result.handles) && result.handles.length === normalized.length &&
      result.handles.every(handle => typeof handle === 'string' && handle.length > 0) &&
      new Set(result.handles).size === result.handles.length;
    if (!validHandles) {
      for (const handle of Array.isArray(result.handles) ? result.handles : []) {
        if (typeof handle === 'string' && handle) {
          try { native.ControlAnimation(handle, 'cancel', 0); } catch (error) { report(error); }
        }
      }
      if (compiledReservationsActive) {
        for (const plan of usedCompiledPlans!) releaseCompiledPlanBinding(plan.key);
        compiledReservationsActive = false;
      }
      throw new Error('Native animation batch returned invalid handles');
    }
    for (const key of new Set(normalized.map(request => request.key))) {
      for (const existing of [...(animationsByTarget.get(key) ?? [])]) existing.settle('replaced');
    }
    const animations = normalized.map((request, index) => {
      const planKey = usedCompiledPlans?.[index].key;
      if (planKey) {
        const entry = compiledPlanCache.get(planKey);
        if (!entry) throw new Error('Compiled animation plan disappeared before binding');
      }
      const animation = new NativeAnimationController(result.handles[index], 'native', request.key, planKey,
        getAnimationPropertyCostClass(request.property));
      addNativeAnimation(animation);
      trackTargetAnimation(request.key, animation);
      return animation;
    });
    compiledReservationsActive = false;
    return animations;
  }
  if (normalized.some(request => request.options.fallback === 'reject' ||
      request.nativeOptions.composite === 'layered-replace') ||
      !['unsupported_property', 'unsupported_transform_value',
      'mixed_transform_primitives', 'unsupported_easing'].includes(result.error || ''))
    throw new Error(result.error || 'Animation was rejected');

  const fallback = normalized.map(request => {
    replaceForFallback(request.key);
    return {
      request,
      frames: fallbackFrames(request.keyframes),
      underlyingValue: native.GetComputedProperty(request.node, request.property),
    };
  });
  const animations = fallback.map(({ request, frames, underlyingValue }) => {
    if (!underlyingValue) throw new Error(`Could not capture underlying ${request.property} value`);
    const handle = `js:${++fallbackSequence}`;
    const options = request.nativeOptions;
    const animation = new JsAnimationController(handle, request.key, request.node, request.property, frames,
      options.duration, options.iterations, options.direction, options.fill, underlyingValue,
      options.delay, options.playbackRate);
    fallbackAnimations.set(handle, animation);
    trackTargetAnimation(request.key, animation);
    return animation;
  });
  requestFallbackFrame();
  return animations;
}

export function startAnimation(
  node: number,
  property: string,
  keyframes: RmlAnimationKeyframe[],
  options: RmlAnimationOptions,
): RmlAnimation {
  return startAnimations([{ node, property, keyframes, options }])[0];
}

export function disposeAnimations(): void {
  disposed = true;
  if (frameRequest) cancelAnimationFrame(frameRequest);
  frameRequest = 0;
  const nativeControllers = [...nativeAnimations.values()].flatMap(lowWords => [...lowWords.values()]);
  for (const animation of nativeControllers) {
    try { animation.cancel(); } catch (error) { report(error); }
  }
  for (const animation of [...fallbackAnimations.values()]) animation.cancel();
  for (const animation of nativeControllers) animation.settle('cancelled');
  try { releaseCompiledPlans(); } catch (error) { report(error); }
  nativeAnimations.clear(); fallbackAnimations.clear(); animationsByTarget.clear();
  if (nativeAnimationEventPackedDelegate)
    nativeAnimationEventPackedDelegate.Remove(onNativeAnimationEventsPacked);
  else
    nativeAnimationEventDelegate.Remove(onNativeAnimationEvents);
}

if (nativeAnimationEventPackedDelegate)
  nativeAnimationEventPackedDelegate.Add(onNativeAnimationEventsPacked);
else
  nativeAnimationEventDelegate.Add(onNativeAnimationEvents);
