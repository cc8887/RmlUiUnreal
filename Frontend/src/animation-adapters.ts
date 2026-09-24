import { native } from './bridge';
import {
  startAnimations,
  type RmlAnimation,
  type RmlAnimationCompletion,
  type RmlAnimationDirection,
  type RmlAnimationKeyframe,
  type RmlAnimationOptions,
} from './animation';

export type RmlAnimationTarget = number | string | readonly (number | string)[];
type Scalar = number | string;
type PropertyValue = Scalar | readonly [Scalar, Scalar];

export const RML_ANIMATION_ADAPTER_VERSIONS = Object.freeze({
  animationJs: '0.5.0',
  animeJs: '4.5.0',
  gsap: '3.15.0',
});

export class RmlAnimationAdapterError extends Error {
  constructor(readonly library: string, readonly code: string, message: string) {
    super(`${library}:${code}: ${message}`);
    this.name = 'RmlAnimationAdapterError';
  }
}

export interface RmlAnimationGroup {
  readonly animations: readonly RmlAnimation[];
  readonly finished: Promise<readonly RmlAnimationCompletion[]>;
  pause(): void;
  play(): void;
  seek(localTimeSeconds: number): void;
  setPlaybackRate(rate: number): void;
  cancel(): void;
}

class AnimationGroup implements RmlAnimationGroup {
  readonly finished: Promise<readonly RmlAnimationCompletion[]>;

  constructor(
    readonly animations: readonly RmlAnimation[],
    onComplete?: () => void,
    onInterrupt?: () => void,
  ) {
    this.finished = Promise.all(animations.map(animation => animation.finished)).then(results => {
      if (results.every(result => result.reason === 'completed')) onComplete?.();
      else onInterrupt?.();
      return results;
    });
  }

  pause(): void { for (const animation of this.animations) animation.pause(); }
  play(): void { for (const animation of this.animations) animation.play(); }
  seek(value: number): void { for (const animation of this.animations) animation.seek(value); }
  setPlaybackRate(value: number): void { for (const animation of this.animations) animation.setPlaybackRate(value); }
  cancel(): void { for (const animation of this.animations) animation.cancel(); }
}

export interface RmlAnimationCompilePlan {
  node: number;
  property: string;
  keyframes: RmlAnimationKeyframe[];
  options: RmlAnimationOptions;
}

export interface RmlAnimationSourceFrame {
  offset: number;
  value: unknown;
  easing?: string;
}

export interface RmlAnimationSourceTrack {
  sourceName: string;
  frames: readonly RmlAnimationSourceFrame[];
}

export interface RmlAnimationSourceStagger {
  each: number;
  from?: 'start' | 'end';
  base?: number;
}

export interface RmlAnimationSourceRequest {
  library: string;
  targets: RmlAnimationTarget;
  tracks: readonly RmlAnimationSourceTrack[];
  options: RmlAnimationOptions;
  defaultEasing: string;
  stagger?: RmlAnimationSourceStagger;
}

export type RmlAnimationTimelineAnchor =
  | 'absolute'
  | 'timeline-end'
  | 'previous-start'
  | 'previous-actual-start'
  | 'previous-end'
  | 'label';

export interface RmlAnimationTimelinePositionExpression {
  anchor: RmlAnimationTimelineAnchor;
  value?: number;
  label?: string;
  scale?: number;
  offset?: number;
}

export interface RmlAnimationTimelineStep {
  source: RmlAnimationSourceRequest;
  position: RmlAnimationTimelinePositionExpression;
  previousTarget?: 'group' | 'last-target';
  overlap?: 'reject' | 'replace' | 'layered-replace';
}

export interface RmlAnimationTimelineLabelEntry {
  label: string;
  position: RmlAnimationTimelinePositionExpression;
}

export type RmlAnimationTimelineEntry = RmlAnimationTimelineStep | RmlAnimationTimelineLabelEntry;

const currentSourceValue = Symbol('rml-current-animation-value');

export interface RmlAnimationHostMetrics {
  visible: boolean;
  x: number; y: number; width: number; height: number;
  layoutX: number; layoutY: number; layoutWidth: number; layoutHeight: number;
  scrollTop: number; scrollLeft: number; scrollWidth: number; scrollHeight: number;
  clientWidth: number; clientHeight: number;
  clipX: number; clipY: number; clipWidth: number; clipHeight: number;
}

export interface RmlAnimationHostSnapshot {
  accepted: boolean;
  revision: string;
  viewport: { width: number; height: number; dpi: number };
  nodes: Array<{ node: number; properties: Record<string, string>; metrics?: RmlAnimationHostMetrics }>;
  targetGroups: number[][];
  error?: string;
}

const transformAliases = new Set([
  'x', 'y', 'translatex', 'translatey', 'xpercent', 'ypercent',
  'scale', 'rotate', 'rotation', 'skew', 'skewx', 'skewy',
]);
const unitlessProperties = new Set(['opacity', 'z-index', 'font-weight', 'flex-grow', 'flex-shrink']);

function fail(library: string, code: string, message: string): never {
  throw new RmlAnimationAdapterError(library, code, message);
}

function finiteNumber(library: string, name: string, value: unknown, fallback: number, allowZero = true): number {
  const result = value === undefined ? fallback : value;
  if (typeof result !== 'number' || !Number.isFinite(result) || result < 0 || (!allowZero && result === 0))
    fail(library, 'invalid_timing', `${name} must be ${allowZero ? 'a non-negative' : 'a positive'} finite number`);
  return result;
}

export function captureAnimationHostSnapshot(
  targets: RmlAnimationTarget,
  properties: readonly string[],
  includeMetrics = false,
): RmlAnimationHostSnapshot {
  const targetList = Array.isArray(targets) ? [...targets] : [targets];
  const propertyList = [...new Set(properties.map(property => property.trim().toLowerCase()))];
  const snapshot = JSON.parse(native.ResolveAnimationHostSnapshot(JSON.stringify({
    targets: targetList,
    properties: propertyList,
    includeMetrics,
  }))) as RmlAnimationHostSnapshot;
  if (!snapshot.accepted) throw new Error(snapshot.error || 'Animation HostSnapshot was rejected');
  if (!Array.isArray(snapshot.nodes) || !snapshot.nodes.length) throw new Error('Animation HostSnapshot returned no nodes');
  if (!Array.isArray(snapshot.targetGroups) || snapshot.targetGroups.length !== targetList.length ||
      snapshot.targetGroups.some(group => !Array.isArray(group) || !group.length ||
        group.some(node => !Number.isInteger(node) || node <= 0)))
    throw new Error('Animation HostSnapshot returned invalid target groups');
  return snapshot;
}

function adapterSnapshot(
  library: string,
  targets: RmlAnimationTarget,
  properties: readonly string[],
): RmlAnimationHostSnapshot {
  try {
    return captureAnimationHostSnapshot(targets, properties, true);
  } catch (error) {
    return fail(library, 'host_snapshot_failed', error instanceof Error ? error.message : String(error));
  }
}

function cssName(name: string): string {
  return name.replace(/([a-z0-9])([A-Z])/g, '$1-$2').toLowerCase();
}

function normalizedProperty(name: string): string {
  const lower = name.toLowerCase();
  if (lower === 'alpha') return 'opacity';
  if (transformAliases.has(lower)) return 'transform';
  return cssName(name);
}

function scalar(library: string, property: string, value: unknown): Scalar {
  if (typeof value !== 'number' && typeof value !== 'string')
    fail(library, 'unsupported_value', `${property} requires a finite number or string`);
  if (typeof value === 'number' && !Number.isFinite(value)) fail(library, 'unsupported_value', `${property} is not finite`);
  return value;
}

const relativeValuePattern = /^([+\-*/])=\s*(-?(?:\d+(?:\.\d*)?|\.\d+))\s*([a-z%]*)$/i;
const numericValuePattern = /^\s*(-?(?:\d+(?:\.\d*)?|\.\d+))\s*([a-z%]*)\s*$/i;

function isRelativeValue(value: unknown): boolean {
  return typeof value === 'string' && relativeValuePattern.test(value.trim());
}

function parseNumericValue(library: string, property: string, value: Scalar): { number: number; unit: string } {
  if (typeof value === 'number') return { number: value, unit: '' };
  const match = value.match(numericValuePattern);
  if (!match) fail(library, 'unsupported_relative_base', `${property} has a non-numeric relative base: ${value}`);
  return { number: Number(match[1]), unit: match[2].toLowerCase() };
}

function resolveRelativeValue(library: string, property: string, value: Scalar, base: Scalar | undefined): Scalar {
  if (!isRelativeValue(value)) return value;
  if (base === undefined) fail(library, 'missing_initial_value', `No relative base is available for ${property}`);
  const match = String(value).trim().match(relativeValuePattern)!;
  const left = parseNumericValue(library, property, base);
  const right = Number(match[2]);
  const rightUnit = match[3].toLowerCase();
  const operator = match[1];
  if (operator === '/' && !library.startsWith('gsap@'))
    fail(library, 'unsupported_relative_operator', `${property} division is not supported by ${library}`);
  if ((operator === '*' || operator === '/') && rightUnit)
    fail(library, 'relative_unit_mismatch', `${property} multiplication and division require a unitless operand`);
  if ((operator === '+' || operator === '-') && left.unit && rightUnit && left.unit !== rightUnit)
    fail(library, 'relative_unit_mismatch', `${property} cannot combine ${left.unit} and ${rightUnit}`);
  if (operator === '/' && right === 0) fail(library, 'invalid_relative_value', `${property} cannot divide by zero`);
  const rawResult = operator === '+' ? left.number + right
    : operator === '-' ? left.number - right
      : operator === '*' ? left.number * right
        : left.number / right;
  if (!Number.isFinite(rawResult)) fail(library, 'invalid_relative_value', `${property} produced a non-finite value`);
  const result = Number(rawResult.toFixed(12));
  const unit = operator === '+' || operator === '-' ? (left.unit || rightUnit) : left.unit;
  return unit ? `${result}${unit}` : result;
}

function cssValue(property: string, value: Scalar): string | number {
  if (typeof value === 'string' || unitlessProperties.has(property)) return value;
  return `${value}px`;
}

function transformValue(library: string, sourceName: string, value: Scalar): string {
  const name = sourceName.toLowerCase();
  const text = String(value).trim();
  if (name === 'scale') return `scale(${text})`;
  if (name === 'rotate' || name === 'rotation') return `rotate(${typeof value === 'number' ? `${value}deg` : text})`;
  if (name === 'skew' || name === 'skewx') return `skewX(${typeof value === 'number' ? `${value}deg` : text})`;
  if (name === 'skewy') return `skewY(${typeof value === 'number' ? `${value}deg` : text})`;
  if (name === 'x' || name === 'translatex' || name === 'xpercent') return `translate(${typeof value === 'number' ? `${value}px` : text},0px)`;
  if (name === 'y' || name === 'translatey' || name === 'ypercent') return `translate(0px,${typeof value === 'number' ? `${value}px` : text})`;
  return fail(library, 'unsupported_transform', `Unsupported transform alias ${sourceName}`);
}

function transformBaseValue(library: string, sourceName: string, computed: string | undefined): Scalar {
  const name = sourceName.toLowerCase();
  const text = (computed || '').trim().toLowerCase();
  if (!text || text === 'none') {
    if (name === 'scale') return 1;
    if (name === 'rotate' || name === 'rotation' || name.startsWith('skew')) return '0deg';
    return '0px';
  }
  if (name === 'scale') {
    const match = text.match(/^scale\(\s*([^,)]+)\s*\)$/);
    if (match) return match[1];
  } else if (name === 'rotate' || name === 'rotation' || name.startsWith('skew')) {
    const match = text.match(/^(?:rotate|skewx|skewy)\(\s*([^)]+)\s*\)$/);
    if (match) return match[1];
  } else {
    const match = text.match(/^translate\(\s*([^,]+)\s*,\s*([^)]+)\s*\)$/);
    if (match) return name === 'y' || name === 'translatey' ? match[2] : match[1];
  }
  return fail(library, 'unsupported_relative_transform_base', `${sourceName} cannot resolve relative value from ${computed}`);
}

function sourceTrackFrames(
  library: string,
  sourceName: string,
  sourceFrames: readonly RmlAnimationSourceFrame[],
  defaultEasing: string,
  initialValue: string | undefined,
): RmlAnimationKeyframe[] {
  const property = normalizedProperty(sourceName);
  const isTransform = property === 'transform';
  if (!sourceFrames.length) fail(library, 'missing_keyframes', `${sourceName} has no keyframes`);
  let previousOffset = -1;
  for (const frame of sourceFrames) {
    if (!Number.isFinite(frame.offset) || frame.offset < 0 || frame.offset > 1 || frame.offset <= previousOffset)
      fail(library, 'invalid_keyframe_offset', `${sourceName} offsets must increase strictly within 0..1`);
    previousOffset = frame.offset;
  }
  if (sourceFrames[sourceFrames.length - 1].offset !== 1)
    fail(library, 'incomplete_keyframes', `${sourceName} must contain a final keyframe at 100%`);

  const needsBase = sourceFrames[0].offset > 0 || isRelativeValue(sourceFrames[0].value) ||
    sourceFrames.some(frame => frame.value === currentSourceValue);
  let previous: Scalar | undefined;
  if (needsBase) {
    if (initialValue === undefined || initialValue === '')
      fail(library, 'missing_initial_value', `No computed value is available for ${property}`);
    previous = isTransform ? transformBaseValue(library, sourceName, initialValue) : scalar(library, sourceName, initialValue);
  }
  const frames: RmlAnimationKeyframe[] = [];
  if (sourceFrames[0].offset > 0) {
    frames.push({
      offset: 0,
      value: isTransform ? transformValue(library, sourceName, previous!) : cssValue(property, previous!),
    });
  }
  for (const sourceFrame of sourceFrames) {
    const resolved = sourceFrame.value === currentSourceValue
      ? previous
      : resolveRelativeValue(
        library,
        sourceName,
        scalar(library, sourceName, sourceFrame.value),
        previous,
      );
    if (resolved === undefined)
      fail(library, 'missing_initial_value', `No current value is available for ${property}`);
    const frame: RmlAnimationKeyframe = {
      offset: sourceFrame.offset,
      value: isTransform ? transformValue(library, sourceName, resolved) : cssValue(property, resolved),
    };
    if (frames.length) frames[frames.length - 1].easing = sourceFrame.easing || defaultEasing;
    frames.push(frame);
    previous = resolved;
  }
  if (frames.length < 2) fail(library, 'missing_keyframes', `${sourceName} requires at least two resolved keyframes`);
  return frames;
}

function normalizePowerEasing(library: string, source: unknown, fallback: string): string {
  if (source === undefined || source === null || source === '') source = fallback;
  if (typeof source !== 'string') fail(library, 'unsupported_easing', 'Function and object easings require the JS callback route');
  const text = source.trim();
  const lower = text.toLowerCase();
  if (['linear', 'none', 'ease', 'ease-in', 'ease-out', 'ease-in-out', 'step-start', 'step-end'].includes(lower) ||
      lower.startsWith('cubic-bezier(') || lower.startsWith('steps('))
    return lower === 'none' ? 'linear' : lower;
  const anime = lower.match(/^(in|out|inout)\(\s*([1-9](?:\.\d+)?)\s*\)$/);
  if (anime) return `rml-power(${anime[1]},${anime[2]})`;
  const gsap = lower.match(/^power([1-4])(?:\.(in|out|inout))?$/);
  if (gsap) return `rml-power(${gsap[2] || 'out'},${Number(gsap[1]) + 1})`;
  const animation = text.match(/^ease(In|Out|InOut)(Quad|Cubic|Quart|Quint)$/);
  if (animation) {
    const powers: Record<string, number> = { Quad: 2, Cubic: 3, Quart: 4, Quint: 5 };
    return `rml-power(${animation[1].toLowerCase()},${powers[animation[2]]})`;
  }
  return fail(library, 'unsupported_easing', `Easing cannot be represented exactly: ${text}`);
}

function validateSourceRequest(request: RmlAnimationSourceRequest): void {
  const { library, tracks } = request;
  const transforms = tracks.filter(track => transformAliases.has(track.sourceName.toLowerCase()));
  const components = new Set<string>();
  for (const track of transforms) {
    const name = track.sourceName.toLowerCase();
    const component = name === 'x' || name === 'translatex' || name === 'xpercent' ? 'x'
      : name === 'y' || name === 'translatey' || name === 'ypercent' ? 'y'
        : name === 'rotation' ? 'rotate' : name === 'skew' ? 'skewx' : name;
    if (components.has(component))
      fail(library, 'unsupported_transform_composition', `Transform component ${component} is specified more than once`);
    components.add(component);
  }
  if (!tracks.length) fail(library, 'missing_properties', 'No animatable properties were provided');
}

function composeTransformFrames(
  library: string,
  tracks: readonly { sourceName: string; frames: RmlAnimationKeyframe[] }[],
): RmlAnimationKeyframe[] {
  const reference = tracks[0].frames;
  if (tracks.some(track => track.frames.length !== reference.length || track.frames.some((frame, index) =>
    frame.offset !== reference[index].offset || (frame.easing || '') !== (reference[index].easing || ''))))
    fail(library, 'unsupported_transform_timing_composition',
      'Transform components require identical keyframe offsets and easing');
  return reference.map((frame, index) => {
    let x = '0px', y = '0px', scaleX = '1', scaleY = '1', rotation = '0deg', skewX = '0deg', skewY = '0deg';
    for (const track of tracks) {
      const name = track.sourceName.toLowerCase();
      const value = String(track.frames[index].value).trim();
      if (name === 'x' || name === 'translatex') {
        const match = value.match(/^translate\(([^,]+),\s*0px\)$/);
        if (!match) fail(library, 'unsupported_transform_composition', `Cannot compose ${track.sourceName}`);
        x = match[1];
      } else if (name === 'y' || name === 'translatey') {
        const match = value.match(/^translate\(0px,\s*([^\)]+)\)$/);
        if (!match) fail(library, 'unsupported_transform_composition', `Cannot compose ${track.sourceName}`);
        y = match[1];
      } else if (name === 'scale') {
        const match = value.match(/^scale\(([^,\)]+)(?:,\s*([^\)]+))?\)$/);
        if (!match) fail(library, 'unsupported_transform_composition', 'Cannot compose scale');
        scaleX = match[1]; scaleY = match[2] || match[1];
      } else if (name === 'rotate' || name === 'rotation') {
        const match = value.match(/^rotate\(([^\)]+)\)$/);
        if (!match) fail(library, 'unsupported_transform_composition', `Cannot compose ${track.sourceName}`);
        rotation = match[1];
      } else {
        const match = value.match(/^skew([XY])\(([^\)]+)\)$/i);
        if (!match) fail(library, 'unsupported_transform_composition', `Cannot compose ${track.sourceName}`);
        if (match[1].toLowerCase() === 'x') skewX = match[2]; else skewY = match[2];
      }
    }
    const hasSkew = tracks.some(track => track.sourceName.toLowerCase().startsWith('skew'));
    return {
      offset: frame.offset,
      value: `translate(${x},${y}) scale(${scaleX},${scaleY}) rotate(${rotation})${hasSkew ? ` skew(${skewX},${skewY})` : ''}`,
      ...(frame.easing ? { easing: frame.easing } : {}),
    };
  });
}

function requiredSourceProperties(tracks: readonly RmlAnimationSourceTrack[]): string[] {
  return tracks
    .filter(track => track.frames[0]?.offset > 0 || isRelativeValue(track.frames[0]?.value) ||
      track.frames.some(frame => frame.value === currentSourceValue))
    .map(track => normalizedProperty(track.sourceName));
}

type RmlAnimationSnapshotNode = RmlAnimationHostSnapshot['nodes'][number];

function sourceStaggerDelay(request: RmlAnimationSourceRequest, targetIndex: number, targetCount: number): number {
  const stagger = request.stagger;
  if (!stagger) return 0;
  const staggerIndex = stagger.from === 'end' ? targetCount - targetIndex - 1 : targetIndex;
  return (stagger.base ?? 0) + stagger.each * staggerIndex;
}

function compileSourceRequestForNodes(
  request: RmlAnimationSourceRequest,
  nodes: readonly RmlAnimationSnapshotNode[],
  resolvedValues?: Map<string, string>,
): RmlAnimationCompilePlan[] {
  const { library, targets, tracks, options, defaultEasing } = request;
  void targets;
  validateSourceRequest(request);
  const plans: RmlAnimationCompilePlan[] = [];
  for (let targetIndex = 0; targetIndex < nodes.length; ++targetIndex) {
    const target = nodes[targetIndex];
    const staggerDelay = sourceStaggerDelay(request, targetIndex, nodes.length);
    const transformTracks = tracks.filter(track => transformAliases.has(track.sourceName.toLowerCase())).map(track => {
      const name = track.sourceName.toLowerCase();
      if (name !== 'xpercent' && name !== 'ypercent') return track;
      const extent = name === 'xpercent' ? target.metrics?.width : target.metrics?.height;
      if (!Number.isFinite(extent)) fail(library, 'missing_target_metrics', `${track.sourceName} requires target dimensions`);
      return {
        sourceName: name === 'xpercent' ? 'x' : 'y',
        frames: track.frames.map(frame => {
          const percent = typeof frame.value === 'number' ? frame.value : Number(String(frame.value).replace(/%$/, ''));
          if (!Number.isFinite(percent)) fail(library, 'unsupported_percentage_transform', `${track.sourceName} requires numeric percentages`);
          return { ...frame, value: percent * extent! / 100 };
        }),
      };
    });
    const regularTracks = tracks.filter(track => !transformAliases.has(track.sourceName.toLowerCase()));
    const compiledTransforms = transformTracks.map(track => ({
      sourceName: track.sourceName,
      frames: sourceTrackFrames(
        library, track.sourceName, track.frames, defaultEasing,
        resolvedValues?.get(`${target.node}:transform`) ?? target.properties.transform,
      ),
    }));
    if (compiledTransforms.length) {
      const keyframes = compiledTransforms.length === 1
        ? compiledTransforms[0].frames
        : composeTransformFrames(library, compiledTransforms);
      plans.push({
        node: target.node,
        property: 'transform',
        keyframes,
        options: staggerDelay ? { ...options, delay: (options.delay ?? 0) + staggerDelay } : options,
      });
      if (resolvedValues) resolvedValues.set(`${target.node}:transform`, String(keyframes[keyframes.length - 1].value));
    }
    for (const track of regularTracks) {
      const property = normalizedProperty(track.sourceName);
      const targetProperty = `${target.node}:${property}`;
      const keyframes = sourceTrackFrames(
        library,
        track.sourceName,
        track.frames,
        defaultEasing,
        resolvedValues?.get(targetProperty) ?? target.properties[property],
      );
      plans.push({
        node: target.node,
        property,
        keyframes,
        options: staggerDelay ? { ...options, delay: (options.delay ?? 0) + staggerDelay } : options,
      });
      if (resolvedValues) resolvedValues.set(targetProperty, String(keyframes[keyframes.length - 1].value));
    }
  }
  return plans;
}

export function compileAnimationSourceRequest(request: RmlAnimationSourceRequest): RmlAnimationCompilePlan[] {
  validateSourceRequest(request);
  const snapshot = adapterSnapshot(request.library, request.targets, requiredSourceProperties(request.tracks));
  return compileSourceRequestForNodes(request, snapshot.nodes);
}

interface TimelinePlanSegment {
  start: number;
  duration: number;
  plan: RmlAnimationCompilePlan;
  overlap: 'reject' | 'replace' | 'layered-replace';
  order: number;
}

function sameAnimationValue(left: number | string, right: number | string): boolean {
  return String(left).trim().toLowerCase() === String(right).trim().toLowerCase();
}

function normalizedTimelineNumber(value: number): number {
  return Number(value.toFixed(12));
}

function timelineValueCodec(
  library: string,
  value: number | string,
): { values: number[]; signature: string; serialize: (values: number[]) => number | string } {
  if (typeof value === 'number') {
    if (!Number.isFinite(value)) fail(library, 'unsupported_overlap_value', 'Overlap sampling requires finite values');
    return { values: [value], signature: 'scalar:', serialize: values => normalizedTimelineNumber(values[0]) };
  }
  const text = value.trim().toLowerCase();
  const scalarMatch = text.match(numericValuePattern);
  if (scalarMatch) {
    const unit = scalarMatch[2].toLowerCase();
    return {
      values: [Number(scalarMatch[1])],
      signature: `scalar:${unit}`,
      serialize: values => `${normalizedTimelineNumber(values[0])}${unit}`,
    };
  }
  const transformMatch = text.match(/^(scale|rotate|translate)\((.*)\)$/);
  if (!transformMatch)
    return fail(library, 'unsupported_overlap_value', `Cannot sample overlapping value: ${value}`);
  const primitive = transformMatch[1];
  const parts = transformMatch[2].split(',').map(part => part.trim());
  const expectedParts = primitive === 'translate' ? 2 : 1;
  if (parts.length !== expectedParts)
    return fail(library, 'unsupported_overlap_value', `Cannot sample overlapping transform: ${value}`);
  const parsed = parts.map(part => part.match(numericValuePattern));
  if (parsed.some(match => !match))
    return fail(library, 'unsupported_overlap_value', `Cannot sample overlapping transform: ${value}`);
  const units = parsed.map(match => match![2].toLowerCase());
  return {
    values: parsed.map(match => Number(match![1])),
    signature: `transform:${primitive}:${units.join(':')}`,
    serialize: values => `${primitive}(${values.map((component, index) =>
      `${normalizedTimelineNumber(component)}${units[index]}`).join(',')})`,
  };
}

function interpolateTimelineValue(
  library: string,
  left: number | string,
  right: number | string,
  alpha: number,
): number | string {
  if (alpha <= 0) return left;
  if (alpha >= 1) return right;
  const from = timelineValueCodec(library, left);
  const to = timelineValueCodec(library, right);
  if (from.signature !== to.signature || from.values.length !== to.values.length)
    return fail(library, 'unsupported_overlap_value', 'Overlapping values must use one shape and unit');
  return from.serialize(from.values.map((value, index) => value + (to.values[index] - value) * alpha));
}

type CubicBezierPoints = [number, number, number, number];

interface TimelineEasingPrefixSegment {
  offset: number;
  alpha: number;
  easing: string;
}

const cssCubicBezierPresets: Readonly<Record<string, CubicBezierPoints>> = Object.freeze({
  ease: [0.25, 0.1, 0.25, 1],
  'ease-in': [0.42, 0, 1, 1],
  'ease-out': [0, 0, 0.58, 1],
  'ease-in-out': [0.42, 0, 0.58, 1],
});

function cubicBezierCoordinate(t: number, first: number, second: number): number {
  const inverse = 1 - t;
  return 3 * inverse * inverse * t * first + 3 * inverse * t * t * second + t * t * t;
}

function parseTimelineCubicBezier(easing: string): CubicBezierPoints | undefined {
  const text = easing.trim().toLowerCase();
  const preset = cssCubicBezierPresets[text];
  if (preset) return [...preset];
  const match = text.match(/^cubic-bezier\(\s*([^,]+),\s*([^,]+),\s*([^,]+),\s*([^\)]+)\)$/);
  if (!match) return undefined;
  const points = match.slice(1).map(Number) as CubicBezierPoints;
  if (points.some(value => !Number.isFinite(value)) || points[0] < 0 || points[0] > 1 ||
      points[2] < 0 || points[2] > 1)
    return undefined;
  return points;
}

function serializeTimelineCubicBezier(points: CubicBezierPoints): string {
  return `cubic-bezier(${points.map(normalizedTimelineNumber).join(',')})`;
}

function sliceTimelineCubicBezier(
  library: string,
  points: CubicBezierPoints,
  cutoff: number,
): { alpha: number; easing: string } {
  let low = 0;
  let high = 1;
  for (let index = 0; index < 48; ++index) {
    const middle = (low + high) * 0.5;
    if (cubicBezierCoordinate(middle, points[0], points[2]) < cutoff) low = middle;
    else high = middle;
  }
  const parameter = (low + high) * 0.5;
  const firstX = points[0] * parameter;
  const firstY = points[1] * parameter;
  const middleX = points[0] + (points[2] - points[0]) * parameter;
  const middleY = points[1] + (points[3] - points[1]) * parameter;
  const secondX = firstX + (middleX - firstX) * parameter;
  const secondY = firstY + (middleY - firstY) * parameter;
  const endX = cubicBezierCoordinate(parameter, points[0], points[2]);
  const endY = cubicBezierCoordinate(parameter, points[1], points[3]);
  if (Math.abs(endX) <= 1e-12 || Math.abs(endY) <= 1e-12)
    return fail(library, 'unsupported_overlap_easing', 'The easing prefix cannot be normalized without losing motion');
  return {
    alpha: endY,
    easing: serializeTimelineCubicBezier([
      firstX / endX,
      firstY / endY,
      secondX / endX,
      secondY / endY,
    ]),
  };
}

function exactPowerCubic(mode: 'in' | 'out', power: number): CubicBezierPoints | undefined {
  if (power === 2) return mode === 'in'
    ? [1 / 3, 0, 2 / 3, 1 / 3]
    : [1 / 3, 2 / 3, 2 / 3, 1];
  if (power === 3) return mode === 'in'
    ? [1 / 3, 0, 2 / 3, 0]
    : [1 / 3, 1, 2 / 3, 1];
  return undefined;
}

function timelineEasingPrefix(
  library: string,
  easing: string | undefined,
  cutoff: number,
): TimelineEasingPrefixSegment[] {
  const text = (easing ?? 'linear').trim().toLowerCase();
  if (text === 'linear') return [{ offset: cutoff, alpha: cutoff, easing: 'linear' }];
  const cubic = parseTimelineCubicBezier(text);
  if (cubic) {
    const sliced = sliceTimelineCubicBezier(library, cubic, cutoff);
    return [{ offset: cutoff, ...sliced }];
  }
  const powerMatch = text.match(/^rml-power\((in|out|inout),\s*([0-9]+(?:\.[0-9]+)?)\)$/);
  if (!powerMatch)
    return fail(library, 'unsupported_overlap_easing', `Cannot slice easing exactly: ${easing}`);
  const mode = powerMatch[1] as 'in' | 'out' | 'inout';
  const power = Number(powerMatch[2]);
  if (mode !== 'inout') {
    const points = exactPowerCubic(mode, power);
    if (!points)
      return fail(library, 'unsupported_overlap_easing', `Power ${power} ${mode} cannot be represented exactly by cubic-bezier`);
    const sliced = sliceTimelineCubicBezier(library, points, cutoff);
    return [{ offset: cutoff, ...sliced }];
  }
  const inPoints = exactPowerCubic('in', power);
  const outPoints = exactPowerCubic('out', power);
  if (!inPoints || !outPoints)
    return fail(library, 'unsupported_overlap_easing', `Power ${power} inout cannot be represented exactly by cubic-bezier segments`);
  if (cutoff <= 0.5) {
    const sliced = sliceTimelineCubicBezier(library, inPoints, cutoff * 2);
    return [{ offset: cutoff, alpha: sliced.alpha * 0.5, easing: sliced.easing }];
  }
  const sliced = sliceTimelineCubicBezier(library, outPoints, cutoff * 2 - 1);
  return [
    { offset: 0.5, alpha: 0.5, easing: serializeTimelineCubicBezier(inPoints) },
    { offset: cutoff, alpha: 0.5 + sliced.alpha * 0.5, easing: sliced.easing },
  ];
}

function canonicalizeExactPowerEasings(
  library: string,
  plan: RmlAnimationCompilePlan,
): RmlAnimationCompilePlan {
  const keyframes: RmlAnimationKeyframe[] = [];
  for (let index = 0; index < plan.keyframes.length; ++index) {
    const source = plan.keyframes[index];
    const frame = { ...source };
    const match = (frame.easing ?? '').trim().toLowerCase()
      .match(/^rml-power\((in|out|inout),\s*([0-9]+(?:\.[0-9]+)?)\)$/);
    if (!match) {
      keyframes.push(frame);
      continue;
    }
    const mode = match[1] as 'in' | 'out' | 'inout';
    const power = Number(match[2]);
    if (mode !== 'inout') {
      const points = exactPowerCubic(mode, power);
      if (points) frame.easing = serializeTimelineCubicBezier(points);
      keyframes.push(frame);
      continue;
    }
    const inPoints = exactPowerCubic('in', power);
    const outPoints = exactPowerCubic('out', power);
    if (!inPoints || !outPoints) {
      keyframes.push(frame);
      continue;
    }
    const next = plan.keyframes[index + 1];
    if (!next || next.offset <= frame.offset) {
      frame.easing = 'linear';
      keyframes.push(frame);
      continue;
    }
    frame.easing = serializeTimelineCubicBezier(inPoints);
    keyframes.push(frame, {
      offset: normalizedTimelineNumber((frame.offset + next.offset) * 0.5),
      value: interpolateTimelineValue(library, frame.value, next.value, 0.5),
      easing: serializeTimelineCubicBezier(outPoints),
    });
  }
  return { ...plan, keyframes };
}

function sampleTimelinePlan(
  library: string,
  plan: RmlAnimationCompilePlan,
  normalized: number,
): number | string {
  const frames = plan.keyframes;
  if (normalized <= 0) return frames[0].value;
  if (normalized >= 1) return frames[frames.length - 1].value;
  let index = 0;
  while (index + 1 < frames.length - 1 && normalized >= frames[index + 1].offset) ++index;
  const left = frames[index];
  const right = frames[index + 1];
  const span = right.offset - left.offset;
  if (span <= 0) return right.value;
  const alpha = (normalized - left.offset) / span;
  if (alpha <= 1e-9) return left.value;
  if (alpha >= 1 - 1e-9) return right.value;
  const prefix = timelineEasingPrefix(library, left.easing, alpha);
  return interpolateTimelineValue(library, left.value, right.value, prefix[prefix.length - 1].alpha);
}

function sampleTimelineSegment(
  library: string,
  segment: TimelinePlanSegment,
  time: number,
): number | string {
  if (segment.duration === 0) return segment.plan.keyframes[segment.plan.keyframes.length - 1].value;
  return sampleTimelinePlan(library, segment.plan, (time - segment.start) / segment.duration);
}

function truncateTimelineSegment(
  library: string,
  segment: TimelinePlanSegment,
  end: number,
): TimelinePlanSegment | undefined {
  const duration = end - segment.start;
  if (duration <= 1e-9) return undefined;
  const cutoff = duration / segment.duration;
  const source = segment.plan.keyframes;
  let exactIndex = -1;
  for (let index = 0; index < source.length; ++index) {
    if (Math.abs(source[index].offset - cutoff) <= 1e-9) exactIndex = index;
  }
  let keyframes: RmlAnimationKeyframe[];
  if (exactIndex >= 0) {
    keyframes = source.slice(0, exactIndex + 1).map(frame => ({ ...frame }));
  } else {
    let leftIndex = 0;
    while (leftIndex + 1 < source.length && source[leftIndex + 1].offset < cutoff) ++leftIndex;
    const left = source[leftIndex];
    const right = source[leftIndex + 1];
    const span = right.offset - left.offset;
    const segmentCutoff = (cutoff - left.offset) / span;
    const prefix = timelineEasingPrefix(library, left.easing, segmentCutoff);
    keyframes = source.slice(0, leftIndex + 1).map(frame => ({ ...frame }));
    for (const prefixSegment of prefix) {
      keyframes[keyframes.length - 1].easing = prefixSegment.easing;
      keyframes.push({
        offset: left.offset + prefixSegment.offset * span,
        value: interpolateTimelineValue(library, left.value, right.value, prefixSegment.alpha),
      });
    }
  }
  for (const frame of keyframes) frame.offset = normalizedTimelineNumber(frame.offset / cutoff);
  return {
    ...segment,
    duration: normalizedTimelineNumber(duration),
    plan: {
      ...segment.plan,
      keyframes,
      options: { ...segment.plan.options, duration: normalizedTimelineNumber(duration) },
    },
  };
}

function mergeTimelineSegments(
  library: string,
  segments: TimelinePlanSegment[],
  requestedEnd?: number,
): RmlAnimationCompilePlan {
  segments.sort((left, right) => left.start - right.start);
  const epsilon = 1e-9;
  const first = segments[0];
  const end = Math.max(requestedEnd ?? 0, ...segments.map(segment => segment.start + segment.duration));
  if (segments.length === 1) {
    const segmentEnd = first.start + first.duration;
    if (segmentEnd < end - epsilon) {
      const duration = end - first.start;
      const keyframes = first.plan.keyframes.map(frame => ({
        ...frame,
        offset: first.duration === 0 ? 0 : normalizedTimelineNumber(frame.offset * first.duration / duration),
      }));
      const finalValue = first.plan.keyframes[first.plan.keyframes.length - 1].value;
      keyframes[keyframes.length - 1].easing = 'linear';
      keyframes.push({ offset: 1, value: finalValue });
      return {
        ...first.plan,
        keyframes,
        options: {
          ...first.plan.options,
          delay: normalizedTimelineNumber(first.start),
          duration: normalizedTimelineNumber(duration),
          iterations: 1,
          playbackRate: 1,
          direction: 'normal',
        },
      };
    }
    return {
      ...first.plan,
      options: {
        ...first.plan.options,
        delay: normalizedTimelineNumber(first.start),
        duration: normalizedTimelineNumber(first.duration),
      },
    };
  }
  const keyframes: RmlAnimationKeyframe[] = [];
  const trackStart = first.start;
  const trackDuration = end - trackStart;
  if (trackDuration <= epsilon) {
    let previousValue: number | string | undefined;
    for (const segment of segments) {
      const firstFrame = segment.plan.keyframes[0];
      if (previousValue !== undefined && !sameAnimationValue(previousValue, firstFrame.value))
        fail(library, 'discontinuous_timeline_track', `Timeline steps jump for ${segment.plan.node}:${segment.plan.property}`);
      previousValue = segment.plan.keyframes[segment.plan.keyframes.length - 1].value;
    }
    return {
      node: first.plan.node,
      property: first.plan.property,
      keyframes: [
        { ...first.plan.keyframes[0], offset: 0 },
        {
          ...segments[segments.length - 1].plan.keyframes[
            segments[segments.length - 1].plan.keyframes.length - 1
          ],
          offset: 1,
        },
      ],
      options: {
        ...first.plan.options,
        delay: normalizedTimelineNumber(trackStart),
        duration: 0,
        iterations: 1,
        playbackRate: 1,
        direction: 'normal',
        fallback: 'reject',
      },
    };
  }
  let previousEnd = trackStart;
  let previousValue: number | string | undefined;
  for (let segmentIndex = 0; segmentIndex < segments.length; ++segmentIndex) {
    const segment = segments[segmentIndex];
    if (segment.start < previousEnd - epsilon)
      fail(library, 'overlapping_timeline_track', `Timeline steps overlap for ${segment.plan.node}:${segment.plan.property}`);
    const firstFrame = segment.plan.keyframes[0];
    const discontinuous = previousValue !== undefined && !sameAnimationValue(previousValue, firstFrame.value);
    if (discontinuous && segment.overlap !== 'replace' && segment.overlap !== 'layered-replace')
      fail(library, 'discontinuous_timeline_track', `Timeline steps jump for ${segment.plan.node}:${segment.plan.property}`);
    if (segmentIndex > 0) {
      const previousFrame = keyframes[keyframes.length - 1];
      if (segment.start > previousEnd + epsilon) {
        previousFrame.easing = 'linear';
        keyframes.push({
          offset: (segment.start - trackStart) / trackDuration,
          value: previousValue!,
          easing: discontinuous ? 'linear' : firstFrame.easing,
        });
      } else {
        previousFrame.easing = firstFrame.easing;
      }
      if (discontinuous) {
        previousFrame.easing = 'linear';
        keyframes.push({
          ...firstFrame,
          offset: (segment.start - trackStart) / trackDuration,
        });
      }
    }
    for (let frameIndex = segmentIndex === 0 ? 0 : 1; frameIndex < segment.plan.keyframes.length; ++frameIndex) {
      const frame = segment.plan.keyframes[frameIndex];
      keyframes.push({
        ...frame,
        offset: (segment.start + frame.offset * segment.duration - trackStart) / trackDuration,
      });
    }
    previousEnd = segment.start + segment.duration;
    previousValue = segment.plan.keyframes[segment.plan.keyframes.length - 1].value;
  }
  if (previousEnd < end - epsilon) {
    keyframes[keyframes.length - 1].easing = 'linear';
    keyframes.push({ offset: 1, value: previousValue! });
  }
  for (const frame of keyframes) frame.offset = Number(frame.offset.toFixed(12));
  return {
    node: first.plan.node,
    property: first.plan.property,
    keyframes,
    options: {
      ...first.plan.options,
      delay: normalizedTimelineNumber(trackStart),
      duration: normalizedTimelineNumber(trackDuration),
      iterations: 1,
      playbackRate: 1,
      direction: 'normal',
      fallback: segments.some(segment => segment.plan.options.fallback === 'reject') ? 'reject' : 'js-batched',
    },
  };
}

export function compileAnimationTimelineSteps(
  library: string,
  entries: readonly RmlAnimationTimelineEntry[],
): RmlAnimationCompilePlan[] {
  if (!entries.length) fail(library, 'empty_timeline', 'Timeline requires at least one animation step');
  const targetSpecs: Array<number | string> = [];
  const targetRanges: Array<{ start: number; count: number } | undefined> = [];
  const requiredProperties = new Set<string>();
  let stepCount = 0;
  for (const entry of entries) {
    if (!('source' in entry)) {
      targetRanges.push(undefined);
      continue;
    }
    const step = entry;
    validateSourceRequest(step.source);
    if ((step.source.options.iterations ?? 1) !== 1 ||
        (step.source.options.playbackRate ?? 1) !== 1 ||
        (step.source.options.direction ?? 'normal') !== 'normal')
      fail(library, 'unsupported_timeline_playback', 'Timeline steps currently require one normal-speed forward iteration');
    const specs = Array.isArray(step.source.targets) ? [...step.source.targets] : [step.source.targets];
    targetRanges.push({ start: targetSpecs.length, count: specs.length });
    targetSpecs.push(...specs);
    for (const track of step.source.tracks) requiredProperties.add(normalizedProperty(track.sourceName));
    ++stepCount;
  }
  if (!stepCount) fail(library, 'empty_timeline', 'Timeline contains labels but no animation steps');
  const snapshot = adapterSnapshot(library, targetSpecs, [...requiredProperties]);
  const nodesByHandle = new Map(snapshot.nodes.map(node => [node.node, node]));
  const nodesByEntry: RmlAnimationSnapshotNode[][] = entries.map((entry, index) => {
    if (!('source' in entry)) return [];
    const range = targetRanges[index]!;
    const handles: number[] = [];
    const seen = new Set<number>();
    for (let groupIndex = range.start; groupIndex < range.start + range.count; ++groupIndex) {
      for (const node of snapshot.targetGroups[groupIndex]) {
        if (!seen.has(node)) { seen.add(node); handles.push(node); }
      }
    }
    return handles.map(node => nodesByHandle.get(node)).filter((node): node is RmlAnimationSnapshotNode => !!node);
  });

  interface ResolvedTimelineStep {
    step: RmlAnimationTimelineStep;
    index: number;
    start: number;
    nodes: RmlAnimationSnapshotNode[];
  }
  interface TimelineBounds { start: number; actualStart: number; end: number }
  const labels = new Map<string, number>();
  const resolvedSteps: ResolvedTimelineStep[] = [];
  let timelineEnd = 0;
  let previous: TimelineBounds | undefined;
  for (let index = 0; index < entries.length; ++index) {
    const entry = entries[index];
    const expression = entry.position;
    let base: number;
    switch (expression.anchor) {
      case 'absolute': base = expression.value ?? 0; break;
      case 'timeline-end': base = timelineEnd; break;
      case 'previous-start':
        if (!previous) fail(library, 'missing_timeline_anchor', 'Timeline position requires a previous animation');
        base = previous.start;
        break;
      case 'previous-actual-start':
        if (!previous) fail(library, 'missing_timeline_anchor', 'Timeline position requires a previous animation');
        base = previous.actualStart;
        break;
      case 'previous-end':
        if (!previous) fail(library, 'missing_timeline_anchor', 'Timeline position requires a previous animation');
        base = previous.end;
        break;
      case 'label': {
        const label = expression.label === undefined ? undefined : labels.get(expression.label);
        if (label === undefined)
          fail(library, 'unknown_timeline_label', `Unknown timeline label: ${expression.label ?? ''}`);
        base = label;
        break;
      }
    }
    const position = normalizedTimelineNumber(base * (expression.scale ?? 1) + (expression.offset ?? 0));
    if (!Number.isFinite(position) || position < 0)
      fail(library, 'invalid_timeline_position', 'Timeline position resolved to a negative or non-finite value');
    if (!('source' in entry)) {
      if (labels.has(entry.label)) fail(library, 'duplicate_timeline_label', `Duplicate timeline label: ${entry.label}`);
      labels.set(entry.label, position);
      continue;
    }
    const nodes = nodesByEntry[index];
    const baseDelay = entry.source.options.delay ?? 0;
    let actualStart = Number.POSITIVE_INFINITY;
    let end = Number.NEGATIVE_INFINITY;
    let lastActualStart = position + baseDelay;
    let lastEnd = lastActualStart + entry.source.options.duration;
    for (let targetIndex = 0; targetIndex < nodes.length; ++targetIndex) {
      const delay = baseDelay + sourceStaggerDelay(entry.source, targetIndex, nodes.length);
      const targetStart = position + delay;
      const targetEnd = targetStart + entry.source.options.duration;
      actualStart = Math.min(actualStart, targetStart);
      end = Math.max(end, targetEnd);
      if (targetIndex === nodes.length - 1) {
        lastActualStart = targetStart;
        lastEnd = targetEnd;
      }
    }
    actualStart = normalizedTimelineNumber(actualStart);
    end = normalizedTimelineNumber(end);
    previous = entry.previousTarget === 'last-target'
      ? { start: position, actualStart: normalizedTimelineNumber(lastActualStart), end: normalizedTimelineNumber(lastEnd) }
      : { start: position, actualStart, end };
    timelineEnd = Math.max(timelineEnd, end);
    resolvedSteps.push({ step: entry, index, start: position, nodes });
  }

  const resolvedValues = new Map<string, string>();
  const segmentsByTarget = new Map<string, { segments: TimelinePlanSegment[]; end: number }>();
  resolvedSteps.sort((left, right) => left.start - right.start || left.index - right.index);
  for (const { step, index: stepIndex, start: stepStart, nodes } of resolvedSteps) {
    if ((step.overlap ?? 'reject') === 'replace' ||
        (step.overlap ?? 'reject') === 'layered-replace') {
      for (let targetIndex = 0; targetIndex < nodes.length; ++targetIndex) {
        const targetStart = stepStart + (step.source.options.delay ?? 0) +
          sourceStaggerDelay(step.source, targetIndex, nodes.length);
        for (const track of step.source.tracks) {
          const key = `${nodes[targetIndex].node}:${normalizedProperty(track.sourceName)}`;
          const state = segmentsByTarget.get(key);
          const previous = state?.segments[state.segments.length - 1];
          if (!previous) continue;
          if (previous.start > targetStart + 1e-9)
            fail(library, 'unsupported_overlap_order', 'Replacement steps must be chronological for each expanded target');
          const active = [...state!.segments].reverse().find(segment =>
            targetStart >= segment.start - 1e-9 &&
            targetStart <= segment.start + segment.duration + 1e-9);
          resolvedValues.set(key, String(sampleTimelineSegment(
            library, active ?? previous, targetStart)));
        }
      }
    }
    const plans = compileSourceRequestForNodes(step.source, nodes, resolvedValues);
    for (const plan of plans) {
      const duration = plan.options.duration;
      const start = stepStart + (plan.options.delay ?? 0);
      const key = `${plan.node}:${plan.property.trim().toLowerCase()}`;
      let state = segmentsByTarget.get(key);
      if (!state) {
        state = { segments: [], end: start + duration };
        segmentsByTarget.set(key, state);
      } else {
        state.end = Math.max(state.end, start + duration);
      }
      const overlap = step.overlap ?? 'reject';
      const previous = state.segments[state.segments.length - 1];
      if (previous && overlap === 'replace' && start < previous.start + previous.duration - 1e-9) {
        const truncated = truncateTimelineSegment(library, previous, start);
        if (truncated) state.segments[state.segments.length - 1] = truncated;
        else state.segments.pop();
      }
      state.segments.push({ start, duration, plan, overlap, order: stepIndex });
    }
  }
  const plans: RmlAnimationCompilePlan[] = [];
  for (const state of segmentsByTarget.values()) {
    const segments = state.segments;
    const hasLayeredOverlap = segments.some((segment, index) =>
      segment.overlap === 'layered-replace' && segments.slice(0, index).some(previous =>
        segment.start < previous.start + previous.duration - 1e-9));
    if (!hasLayeredOverlap) {
      plans.push(mergeTimelineSegments(library, segments, state.end));
      continue;
    }
    for (const segment of segments) {
      plans.push({
        ...segment.plan,
        options: {
          ...segment.plan.options,
          delay: normalizedTimelineNumber(segment.start),
          duration: normalizedTimelineNumber(segment.duration),
          iterations: 1,
          playbackRate: 1,
          direction: 'normal',
          composite: 'layered-replace',
          compositionOrder: segment.order,
          fallback: 'reject',
        },
      });
    }
  }
  return plans;
}

function propertyTracks(library: string, properties: Record<string, unknown>): RmlAnimationSourceTrack[] {
  return Object.entries(properties).map(([sourceName, input]) => {
    const pair = Array.isArray(input) ? input : undefined;
    if (pair && pair.length !== 2)
      fail(library, 'unsupported_keyframes', `${sourceName} supports only [from, to] outside the keyframes option`);
    return {
      sourceName,
      frames: pair
        ? [{ offset: 0, value: pair[0] }, { offset: 1, value: pair[1] }]
        : [{ offset: 1, value: input }],
    };
  });
}

function instantSourceRequest(
  library: string,
  targets: RmlAnimationTarget,
  properties: Record<string, unknown>,
  metadata: ReadonlySet<string>,
): RmlAnimationSourceRequest {
  if (!properties || typeof properties !== 'object' || Array.isArray(properties))
    fail(library, 'invalid_set_properties', 'Timeline set properties must be an object');
  const entries = Object.entries(properties);
  if (!entries.length) fail(library, 'missing_properties', 'Timeline set requires at least one property');
  for (const [name, value] of entries) {
    if (metadata.has(name))
      fail(library, 'unsupported_set_option', `${name} is not supported by a timeline set`);
    if (Array.isArray(value))
      fail(library, 'unsupported_set_value', `${name} cannot use an array value in a timeline set`);
  }
  return {
    library,
    targets,
    tracks: entries.map(([sourceName, value]) => ({ sourceName, frames: [{ offset: 1, value }] })),
    options: {
      duration: 0,
      delay: 0,
      iterations: 1,
      playbackRate: 1,
      direction: 'normal',
      fill: 'both',
      composite: 'replace',
      fallback: 'reject',
    },
    defaultEasing: 'linear',
  };
}

function buildPlans(
  library: string,
  targets: RmlAnimationTarget,
  properties: Record<string, unknown>,
  options: RmlAnimationOptions,
  easing: string,
  stagger?: RmlAnimationSourceStagger,
): RmlAnimationCompilePlan[] {
  return compileAnimationSourceRequest({
    library, targets, tracks: propertyTracks(library, properties), options, defaultEasing: easing, stagger,
  });
}

function percentageKeyframeTracks(
  library: string,
  input: Record<string, unknown>,
  easingFallback: string,
): RmlAnimationSourceTrack[] {
  const tracks = new Map<string, RmlAnimationSourceFrame[]>();
  const seenOffsets = new Set<number>();
  const entries = Object.entries(input).map(([label, value]) => {
    const match = label.trim().match(/^(100|(?:\d|[1-9]\d)(?:\.\d+)?)%$/);
    if (!match) fail(library, 'invalid_keyframe_offset', `Invalid percentage keyframe: ${label}`);
    const offset = Number(match[1]) / 100;
    if (seenOffsets.has(offset)) fail(library, 'invalid_keyframe_offset', `Duplicate percentage keyframe: ${label}`);
    seenOffsets.add(offset);
    if (!value || typeof value !== 'object' || Array.isArray(value))
      fail(library, 'invalid_keyframe', `${label} must contain a property object`);
    return { offset, value: value as Record<string, unknown> };
  }).sort((a, b) => a.offset - b.offset);
  if (!entries.length) fail(library, 'missing_keyframes', 'The keyframes object is empty');
  for (const entry of entries) {
    const explicitEasing = entry.value.ease;
    if (explicitEasing !== undefined && typeof explicitEasing !== 'string')
      fail(library, 'unsupported_easing', 'Keyframe easing functions require the JS callback route');
    const easing = explicitEasing === undefined
      ? undefined
      : normalizePowerEasing(library, explicitEasing, easingFallback);
    for (const [sourceName, value] of Object.entries(entry.value)) {
      if (sourceName === 'ease') continue;
      if (['duration', 'delay', 'modifier', 'composition'].includes(sourceName))
        fail(library, 'unsupported_keyframe_timing', `${sourceName} is not valid in percentage keyframes`);
      let frames = tracks.get(sourceName);
      if (!frames) { frames = []; tracks.set(sourceName, frames); }
      frames.push({ offset: entry.offset, value, easing });
    }
  }
  for (const [sourceName, frames] of tracks) {
    if (frames[frames.length - 1]?.offset !== 1)
      fail(library, 'incomplete_keyframes', `${sourceName} must have a value at 100%`);
  }
  return [...tracks].map(([sourceName, frames]) => ({ sourceName, frames }));
}

interface SequentialSourceSegment {
  duration: number;
  delay: number;
  easing: string;
  properties: Record<string, unknown>;
}

function sequentialKeyframeTracks(
  library: string,
  segments: readonly SequentialSourceSegment[],
): { tracks: RmlAnimationSourceTrack[]; duration: number } {
  const sourceNames = [...new Set(segments.flatMap(segment => Object.keys(segment.properties)))];
  if (!sourceNames.length) fail(library, 'missing_properties', 'Sequential keyframes contain no animatable properties');
  const duration = segments.reduce((total, segment) => total + segment.delay + segment.duration, 0);
  if (!Number.isFinite(duration) || duration <= 0)
    fail(library, 'invalid_timing', 'Sequential keyframes require a positive total duration');
  const tracks = sourceNames.map(sourceName => {
    let elapsed = 0;
    const frames: RmlAnimationSourceFrame[] = [{ offset: 0, value: currentSourceValue }];
    for (const segment of segments) {
      if (segment.delay > 0) {
        elapsed += segment.delay;
        frames.push({ offset: elapsed / duration, value: currentSourceValue });
      }
      elapsed += segment.duration;
      frames.push({
        offset: elapsed / duration,
        value: Object.prototype.hasOwnProperty.call(segment.properties, sourceName)
          ? segment.properties[sourceName]
          : currentSourceValue,
        easing: segment.easing,
      });
    }
    return { sourceName, frames };
  });
  return { tracks, duration };
}

function normalizeGsapStagger(library: string, input: unknown): RmlAnimationSourceStagger | undefined {
  if (input === undefined || input === 0) return undefined;
  if (typeof input === 'number') {
    const each = finiteNumber(library, 'stagger', input, 0);
    return each ? { each } : undefined;
  }
  if (!input || typeof input !== 'object' || Array.isArray(input))
    return fail(library, 'unsupported_stagger', 'stagger must be a non-negative number or a linear each object');
  const value = input as Record<string, unknown>;
  const allowed = new Set(['each', 'from', 'base']);
  const unsupported = Object.keys(value).filter(key => !allowed.has(key));
  if (unsupported.length)
    fail(library, 'unsupported_stagger_distribution', `Unsupported stagger fields: ${unsupported.join(',')}`);
  const each = finiteNumber(library, 'stagger.each', value.each, 0);
  const base = finiteNumber(library, 'stagger.base', value.base, 0);
  const sourceFrom = value.from ?? 'start';
  const from = sourceFrom === 'end' ? 'end'
    : sourceFrom === 'start' || sourceFrom === 0 ? 'start'
      : fail(library, 'unsupported_stagger_origin', 'Only start/0 and end stagger origins are supported');
  return each || base ? { each, from, base } : undefined;
}

function gsapKeyframeTracks(
  library: string,
  input: unknown,
  varsEase: unknown,
): { tracks: RmlAnimationSourceTrack[]; defaultEasing: string; duration?: number } {
  if (!input || typeof input !== 'object') fail(library, 'invalid_keyframes', 'keyframes must be an object');
  if (Array.isArray(input))
    fail(library, 'unsupported_duration_keyframes', 'Sequential GSAP keyframes require per-segment duration expansion');
  const object = input as Record<string, unknown>;
  const playbackEase = object.ease ?? varsEase ?? 'none';
  if (normalizePowerEasing(library, playbackEase, 'none') !== 'linear')
    fail(library, 'unsupported_keyframe_playback_ease', 'GSAP keyframe playback ease cannot be composed with segment easing');
  const easingSource = object.easeEach ?? 'power1.inOut';
  const defaultEasing = normalizePowerEasing(library, easingSource, 'power1.inOut');
  const values = Object.fromEntries(Object.entries(object).filter(([name]) => name !== 'ease' && name !== 'easeEach'));
  const percentageSyntax = Object.keys(values).some(name => name.trim().endsWith('%'));
  if (percentageSyntax) return { tracks: percentageKeyframeTracks(library, values, String(easingSource)), defaultEasing };
  const tracks = Object.entries(values).map(([sourceName, raw]) => {
    if (!Array.isArray(raw) || raw.length < 2)
      fail(library, 'unsupported_keyframes', `${sourceName} must be an array with at least two values`);
    return {
      sourceName,
      frames: raw.map((value, index) => ({
        offset: index / (raw.length - 1), value,
      })),
    };
  });
  return { tracks, defaultEasing };
}

function runPlans(
  library: string,
  plans: RmlAnimationCompilePlan[],
  onComplete?: () => void,
  onInterrupt?: () => void,
): RmlAnimationGroup {
  const animations = startAnimations(plans.map(plan => canonicalizeExactPowerEasings(library, plan)));
  return new AnimationGroup(animations, onComplete, onInterrupt);
}

export interface AnimationJsOptions {
  el: RmlAnimationTarget;
  draw: Record<string, PropertyValue> | ((time: number, progress: number) => void);
  dur?: number;
  ease?: string;
  loop?: number | boolean;
  pause?: number;
  dir?: 'normal' | 'reverse' | 'alternate';
  defer?: number;
  stagger?: RmlAnimationSourceStagger;
  onFrame?: (...args: unknown[]) => void;
  onDone?: () => void;
}

export function adaptAnimationJs(input: AnimationJsOptions): RmlAnimationGroup {
  const library = 'animationjs@0.5.0';
  if (typeof input.draw === 'function' || input.onFrame) fail(library, 'unsupported_callback', 'draw functions and onFrame require per-frame JS callbacks');
  if (input.loop === true) fail(library, 'unsupported_infinite_loop', 'Infinite loops are not supported');
  const direction = input.dir ?? 'normal';
  const requestedLoops = typeof input.loop === 'number' && input.loop > 0 ? Math.floor(input.loop) : 1;
  const iterations = direction === 'alternate' ? requestedLoops * 2 : requestedLoops;
  if ((input.pause ?? 0) !== 0 && iterations > 1) fail(library, 'unsupported_loop_delay', 'Pause between loops is not represented by the current IR');
  const easing = normalizePowerEasing(library, input.ease, 'linear');
  const durationMilliseconds = finiteNumber(library, 'dur', input.dur, 1000);
  const delayMilliseconds = finiteNumber(library, 'defer', input.defer, 0);
  const options: RmlAnimationOptions = {
    duration: Math.max(1, durationMilliseconds) / 1000,
    delay: delayMilliseconds / 1000,
    iterations,
    direction: direction as RmlAnimationDirection,
    fill: 'both', composite: 'replace', fallback: 'js-batched',
  };
  return runPlans(library, buildPlans(library, input.el, input.draw, options, easing, input.stagger), input.onDone);
}

const animeMetadata = new Set([
  'id', 'keyframes', 'playbackEase', 'playbackRate', 'frameRate', 'loop', 'reversed', 'alternate', 'persist',
  'autoplay', 'duration', 'delay', 'loopDelay', 'ease', 'composition', 'priority', 'modifier', 'onBegin', 'onBeforeUpdate',
  'onUpdate', 'onLoop', 'onPause', 'onComplete', 'onRender',
]);

export interface AnimeJsParams extends Record<string, unknown> {
  duration?: number;
  delay?: number;
  loop?: number | boolean;
  loopDelay?: number;
  reversed?: boolean;
  alternate?: boolean;
  playbackRate?: number;
  priority?: number;
  ease?: string;
  composition?: 'replace' | 0;
  keyframes?: Record<string, Record<string, unknown>> | Array<Record<string, unknown>>;
  onComplete?: () => void;
}

export type AnimeTimelinePositionValue = number | string;

export interface AnimeTimelinePositionStagger {
  readonly kind: 'rml-anime-timeline-position-stagger';
  readonly each: number;
  readonly start?: AnimeTimelinePositionValue;
  readonly from?: 'start' | 'end' | 'first' | 'last';
}

export type AnimeTimelinePosition = AnimeTimelinePositionValue | AnimeTimelinePositionStagger;

export function animeTimelinePositionStagger(
  each: number,
  options: Omit<AnimeTimelinePositionStagger, 'kind' | 'each'> = {},
): AnimeTimelinePositionStagger {
  if (!Number.isFinite(each) || each < 0)
    fail('animejs@4.5.0', 'invalid_timeline_stagger', 'Timeline position stagger each must be non-negative milliseconds');
  return Object.freeze({ kind: 'rml-anime-timeline-position-stagger', each, ...options });
}

export interface AnimeTimelineTween {
  targets: RmlAnimationTarget;
  params: AnimeJsParams;
  position?: AnimeTimelinePosition;
}

export interface AnimeTimelineSet {
  targets: RmlAnimationTarget;
  set: Record<string, unknown>;
  position?: AnimeTimelinePosition;
}

export interface AnimeTimelineLabel {
  label: string;
  position?: AnimeTimelinePositionValue;
}

export interface AnimeTimelineOptions extends Record<string, unknown> {
  defaults?: AnimeJsParams;
  autoplay?: boolean;
  onComplete?: () => void;
}

function animeDurationKeyframeTracks(
  library: string,
  input: readonly Record<string, unknown>[],
  parentDuration: number,
  parentDelay: number,
  parentEasing: string,
): { tracks: RmlAnimationSourceTrack[]; duration: number; delay: number } {
  if (!input.length) fail(library, 'missing_keyframes', 'The duration keyframes array is empty');
  const allowedMetadata = new Set(['duration', 'delay', 'ease', 'composition']);
  const segments = input.map((frame, index): SequentialSourceSegment => {
    if (!frame || typeof frame !== 'object' || Array.isArray(frame))
      return fail(library, 'invalid_keyframe', `Duration keyframe ${index} must be an object`);
    for (const name of Object.keys(frame)) {
      if (animeMetadata.has(name) && !allowedMetadata.has(name))
        fail(library, 'unsupported_keyframe_option', `${name} is not supported inside a duration keyframe`);
    }
    if (frame.composition !== undefined && frame.composition !== 'replace' && frame.composition !== 0)
      fail(library, 'unsupported_composition', 'Only replace composition is supported in duration keyframes');
    const duration = finiteNumber(
      library, `keyframes[${index}].duration`, frame.duration, parentDuration / input.length, false,
    );
    const delay = index === 0 ? 0 : finiteNumber(library, `keyframes[${index}].delay`, frame.delay, 0);
    const easing = normalizePowerEasing(library, frame.ease, parentEasing);
    const properties = Object.fromEntries(Object.entries(frame).filter(([name]) => !animeMetadata.has(name)));
    return { duration, delay, easing, properties };
  });
  const compiled = sequentialKeyframeTracks(library, segments);
  const delay = finiteNumber(library, 'keyframes[0].delay', input[0].delay, parentDelay);
  return { ...compiled, delay };
}

function animeSourceRequest(targets: RmlAnimationTarget, params: AnimeJsParams): RmlAnimationSourceRequest {
  const library = 'animejs@4.5.0';
  for (const callback of ['modifier', 'onBegin', 'onBeforeUpdate', 'onUpdate', 'onLoop', 'onPause', 'onRender']) {
    if (params[callback] !== undefined) fail(library, 'unsupported_callback', `${callback} requires the JS callback route`);
  }
  if (params.autoplay !== undefined && params.autoplay !== true) fail(library, 'unsupported_autoplay', 'Only immediate autoplay is supported');
  if (params.persist === true) fail(library, 'unsupported_persist', 'Persisted replaced animations need an effect stack');
  if (params.playbackEase !== undefined) fail(library, 'unsupported_playback_ease', 'playbackEase is not represented by the current IR');
  if (params.frameRate !== undefined) fail(library, 'unsupported_frame_rate', 'Per-animation frameRate is not represented by the shared scheduler');
  if (params.priority !== undefined) fail(library, 'unsupported_priority', 'Per-animation priority is not represented by the shared scheduler');
  if (params.loop === true || params.loop === Infinity || (typeof params.loop === 'number' && params.loop < 0))
    fail(library, 'unsupported_infinite_loop', 'Infinite loops are not supported');
  const repeats = typeof params.loop === 'number' ? Math.max(0, Math.floor(params.loop)) : 0;
  if ((params.loopDelay ?? 0) !== 0 && repeats > 0) fail(library, 'unsupported_loop_delay', 'loopDelay is not represented by the current IR');
  if (params.composition !== undefined && params.composition !== 'replace' && params.composition !== 0)
    fail(library, 'unsupported_composition', 'Only replace composition is supported');
  const durationMilliseconds = finiteNumber(library, 'duration', params.duration, 1000, false);
  const delayMilliseconds = finiteNumber(library, 'delay', params.delay, 0);
  const direction: RmlAnimationDirection = params.alternate
    ? (params.reversed ? 'alternate-reverse' : 'alternate')
    : (params.reversed ? 'reverse' : 'normal');
  const options: RmlAnimationOptions = {
    duration: durationMilliseconds / 1000,
    delay: delayMilliseconds / 1000,
    iterations: repeats + 1,
    playbackRate: params.playbackRate ?? 1,
    direction, fill: 'both', composite: 'replace', fallback: 'js-batched',
  };
  const properties = Object.fromEntries(Object.entries(params).filter(([name]) => !animeMetadata.has(name)));
  const easing = normalizePowerEasing(library, params.ease, 'out(2)');
  if (params.keyframes !== undefined) {
    if (Object.keys(properties).length)
      fail(library, 'unsupported_mixed_keyframes', 'Direct properties cannot be combined with the keyframes option');
    if (Array.isArray(params.keyframes)) {
      const compiled = animeDurationKeyframeTracks(
        library,
        params.keyframes,
        durationMilliseconds,
        delayMilliseconds,
        String(params.ease ?? 'out(2)'),
      );
      return {
        library, targets, tracks: compiled.tracks,
        options: { ...options, duration: compiled.duration / 1000, delay: compiled.delay / 1000 },
        defaultEasing: easing,
      };
    }
    const tracks = percentageKeyframeTracks(library, params.keyframes, String(params.ease ?? 'out(2)'));
    return {
      library, targets, tracks, options, defaultEasing: easing,
    };
  }
  return {
    library,
    targets,
    tracks: propertyTracks(library, properties),
    options,
    defaultEasing: easing,
  };
}

export function adaptAnimeJs(targets: RmlAnimationTarget, params: AnimeJsParams): RmlAnimationGroup {
  const library = 'animejs@4.5.0';
  return runPlans(library, compileAnimationSourceRequest(animeSourceRequest(targets, params)), params.onComplete);
}

function animeTimelineRelativeExpression(
  anchor: RmlAnimationTimelineAnchor,
  operator: string,
  operand: number,
  label?: string,
): RmlAnimationTimelinePositionExpression {
  return {
    anchor,
    ...(label === undefined ? {} : { label }),
    ...(operator === '*' ? { scale: operand } : { offset: (operator === '-' ? -1 : 1) * operand / 1000 }),
  };
}

function parseAnimeTimelinePosition(
  library: string,
  position: AnimeTimelinePositionValue | undefined,
  hasPrevious: boolean,
  labels: ReadonlySet<string>,
): RmlAnimationTimelinePositionExpression {
  if (position === undefined) return { anchor: 'timeline-end' };
  if (typeof position === 'number') {
    if (!Number.isFinite(position) || position < 0)
      return fail(library, 'invalid_timeline_position', 'Timeline positions must be non-negative finite milliseconds');
    return { anchor: 'absolute', value: normalizedTimelineNumber(position / 1000) };
  }
  if (typeof position !== 'string')
    return fail(library, 'invalid_timeline_position', 'Timeline position must be milliseconds or a supported position string');
  const text = position.trim();
  if (!text) return fail(library, 'invalid_timeline_position', 'Timeline position cannot be empty');
  const numeric = Number(text);
  if (Number.isFinite(numeric)) {
    if (numeric < 0) return fail(library, 'invalid_timeline_position', `Timeline position is negative: ${text}`);
    return { anchor: 'absolute', value: normalizedTimelineNumber(numeric / 1000) };
  }
  if (labels.has(text)) return { anchor: 'label', label: text };
  const numberPattern = '(\\d+(?:\\.\\d*)?|\\.\\d+)';
  const anchor = text.match(new RegExp(`^(<<|<)(?:([+\\-*])=\\s*${numberPattern})?$`));
  if (anchor) {
    if (!hasPrevious) return fail(library, 'missing_timeline_anchor', `${anchor[1]} requires a previous animation`);
    const base: RmlAnimationTimelineAnchor = anchor[1] === '<<' ? 'previous-actual-start' : 'previous-end';
    return anchor[2]
      ? animeTimelineRelativeExpression(base, anchor[2], Number(anchor[3]))
      : { anchor: base };
  }
  const relative = text.match(new RegExp(`^([+\\-*])=\\s*${numberPattern}$`));
  if (relative)
    return animeTimelineRelativeExpression('timeline-end', relative[1], Number(relative[2]));
  const labelRelative = text.match(new RegExp(`^(.+?)([+\\-*])=\\s*${numberPattern}$`));
  if (labelRelative) {
    const labelName = labelRelative[1].trim();
    if (!labels.has(labelName)) return fail(library, 'unknown_timeline_label', `Unknown timeline label: ${labelName}`);
    return animeTimelineRelativeExpression('label', labelRelative[2], Number(labelRelative[3]), labelName);
  }
  return fail(library, 'unknown_timeline_label', `Unknown timeline label or position syntax: ${text}`);
}

function parseAnimeTimelineTweenPosition(
  library: string,
  input: AnimeTimelinePosition | undefined,
  hasPrevious: boolean,
  labels: ReadonlySet<string>,
): {
  position: RmlAnimationTimelinePositionExpression;
  stagger?: RmlAnimationSourceStagger;
  previousTarget?: 'last-target';
} {
  if (typeof input === 'function')
    return fail(library, 'unsupported_timeline_position_function', 'Use animeTimelinePositionStagger for DOM-independent linear position staggering');
  if (!input || typeof input !== 'object') {
    return { position: parseAnimeTimelinePosition(library, input, hasPrevious, labels) };
  }
  if (input.kind !== 'rml-anime-timeline-position-stagger')
    return fail(library, 'invalid_timeline_stagger', 'Timeline position stagger must come from animeTimelinePositionStagger');
  if (!Number.isFinite(input.each) || input.each < 0)
    return fail(library, 'invalid_timeline_stagger', 'Timeline position stagger each must be non-negative milliseconds');
  if (input.from !== undefined && !['start', 'end', 'first', 'last'].includes(input.from))
    return fail(library, 'unsupported_stagger_origin', 'Timeline position stagger supports first/start and last/end');
  return {
    position: parseAnimeTimelinePosition(library, input.start, hasPrevious, labels),
    stagger: {
      each: input.each / 1000,
      from: input.from === 'end' || input.from === 'last' ? 'end' : 'start',
    },
    previousTarget: 'last-target',
  };
}

export function adaptAnimeTimeline(
  entries: readonly (AnimeTimelineTween | AnimeTimelineSet | AnimeTimelineLabel)[],
  options: AnimeTimelineOptions = {},
): RmlAnimationGroup {
  const library = 'animejs@4.5.0';
  if (!entries.length) fail(library, 'empty_timeline', 'Timeline requires at least one entry');
  for (const callback of ['onBegin', 'onBeforeUpdate', 'onUpdate', 'onLoop', 'onPause', 'onRender']) {
    if (options[callback] !== undefined)
      fail(library, 'unsupported_callback', `${callback} requires the JS callback route`);
  }
  for (const unsupported of [
    'duration', 'delay', 'loopDelay', 'reversed', 'alternate', 'loop', 'frameRate', 'playbackRate',
    'priority', 'playbackEase', 'composition',
  ]) {
    if (options[unsupported] !== undefined)
      fail(library, 'unsupported_timeline_playback', `${unsupported} is not represented on the compiled timeline group`);
  }
  if (options.autoplay !== undefined && typeof options.autoplay !== 'boolean')
    fail(library, 'unsupported_autoplay', 'Timeline autoplay must be a boolean');
  if (options.defaults !== undefined && (!options.defaults || typeof options.defaults !== 'object' || Array.isArray(options.defaults)))
    fail(library, 'invalid_timeline_defaults', 'Timeline defaults must be an Anime.js parameter object');
  const defaults = options.defaults ?? {};
  const labels = new Set<string>();
  const timelineEntries: RmlAnimationTimelineEntry[] = [];
  let hasPrevious = false;
  for (const entry of entries) {
    if ('label' in entry) {
      const label = entry.label.trim();
      if (!label) fail(library, 'invalid_timeline_label', 'Timeline labels cannot be empty');
      if (labels.has(label)) fail(library, 'duplicate_timeline_label', `Duplicate timeline label: ${label}`);
      const position = parseAnimeTimelinePosition(library, entry.position, hasPrevious, labels);
      labels.add(label);
      timelineEntries.push({ label, position });
      continue;
    }
    let source: RmlAnimationSourceRequest;
    if ('set' in entry) {
      source = instantSourceRequest(library, entry.targets, entry.set, animeMetadata);
    } else {
      const params: AnimeJsParams = { ...defaults, ...entry.params };
      if (params.onComplete !== undefined)
        fail(library, 'unsupported_timeline_child_callback', 'Timeline child completion callbacks cannot survive track merging');
      source = animeSourceRequest(entry.targets, params);
    }
    const parsedPosition = parseAnimeTimelineTweenPosition(library, entry.position, hasPrevious, labels);
    timelineEntries.push({
      position: parsedPosition.position,
      source: parsedPosition.stagger ? { ...source, stagger: parsedPosition.stagger } : source,
      previousTarget: parsedPosition.previousTarget,
      overlap: 'replace',
    });
    hasPrevious = true;
  }
  if (!hasPrevious) fail(library, 'empty_timeline', 'Timeline contains labels but no animation steps');
  const group = runPlans(library, compileAnimationTimelineSteps(library, timelineEntries), options.onComplete);
  if (options.autoplay === false) group.pause();
  return group;
}

const gsapMetadata = new Set([
  'data', 'id', 'inherit', 'paused', 'repeat', 'repeatDelay', 'repeatRefresh', 'reversed', 'yoyo', 'delay',
  'duration', 'ease', 'endArray', 'immediateRender', 'lazy', 'keyframes', 'onInterrupt', 'overwrite', 'runBackwards',
  'stagger', 'startAt', 'yoyoEase', 'easeReverse', 'callbackScope', 'onComplete', 'onCompleteParams', 'onRepeat',
  'onRepeatParams', 'onReverseComplete', 'onReverseCompleteParams', 'onStart', 'onStartParams', 'onUpdate', 'onUpdateParams',
  'defaults', 'parent', 'scrollTrigger',
  'autoAlpha',
]);

export interface GsapVars extends Record<string, unknown> {
  duration?: number;
  delay?: number;
  repeat?: number;
  repeatDelay?: number;
  reversed?: boolean;
  yoyo?: boolean;
  ease?: string;
  stagger?: number | object;
  keyframes?: Record<string, unknown> | Array<Record<string, unknown>>;
  autoAlpha?: number;
  onComplete?: () => void;
  onInterrupt?: () => void;
}

export type GsapTimelinePosition = number | string;

export interface GsapTimelineTween {
  targets: RmlAnimationTarget;
  vars: GsapVars;
  position?: GsapTimelinePosition;
}

export interface GsapTimelineSet {
  targets: RmlAnimationTarget;
  set: Record<string, unknown>;
  position?: GsapTimelinePosition;
}

export interface GsapTimelineLabel {
  label: string;
  position?: GsapTimelinePosition;
}

export interface GsapTimelineOptions {
  paused?: boolean;
  onComplete?: () => void;
  onInterrupt?: () => void;
}

function gsapSequentialKeyframeTracks(
  library: string,
  input: readonly Record<string, unknown>[],
  requestedDuration: number | undefined,
  playbackEase: unknown,
): { tracks: RmlAnimationSourceTrack[]; duration: number; defaultEasing: string } {
  if (!input.length) fail(library, 'missing_keyframes', 'The sequential keyframes array is empty');
  if (normalizePowerEasing(library, playbackEase, 'none') !== 'linear')
    fail(library, 'unsupported_keyframe_playback_ease', 'GSAP keyframe playback ease cannot be composed with segment easing');
  const allowedMetadata = new Set(['duration', 'delay', 'ease']);
  const segments = input.map((frame, index): SequentialSourceSegment => {
    if (!frame || typeof frame !== 'object' || Array.isArray(frame))
      return fail(library, 'invalid_keyframe', `Sequential keyframe ${index} must be an object`);
    for (const name of Object.keys(frame)) {
      if (gsapMetadata.has(name) && !allowedMetadata.has(name))
        fail(library, 'unsupported_keyframe_option', `${name} is not supported inside a sequential keyframe`);
    }
    return {
      duration: finiteNumber(library, `keyframes[${index}].duration`, frame.duration, 0.5, false),
      delay: finiteNumber(library, `keyframes[${index}].delay`, frame.delay, 0),
      easing: normalizePowerEasing(library, frame.ease, 'none'),
      properties: Object.fromEntries(Object.entries(frame).filter(([name]) => !gsapMetadata.has(name))),
    };
  });
  const compiled = sequentialKeyframeTracks(library, segments);
  return {
    tracks: compiled.tracks,
    duration: requestedDuration ?? compiled.duration,
    defaultEasing: 'linear',
  };
}

function gsapSourceRequest(
  targets: RmlAnimationTarget,
  from: Record<string, unknown> | undefined,
  vars: GsapVars,
): RmlAnimationSourceRequest {
  const library = 'gsap@3.15.0';
  for (const callback of ['onUpdate', 'onRepeat', 'onStart', 'onReverseComplete']) {
    if (vars[callback] !== undefined) fail(library, 'unsupported_callback', `${callback} requires the JS callback route`);
  }
  for (const unsupported of [
    'startAt', 'runBackwards', 'repeatRefresh', 'easeReverse', 'yoyoEase', 'callbackScope',
    'defaults', 'parent', 'scrollTrigger',
  ]) {
    if (vars[unsupported] !== undefined && vars[unsupported] !== false)
      fail(library, `unsupported_${cssName(unsupported).replace(/-/g, '_')}`, `${unsupported} is not represented by the current adapter`);
  }
  for (const callbackParams of ['onCompleteParams', 'onInterruptParams', 'onRepeatParams', 'onStartParams', 'onReverseCompleteParams']) {
    if (vars[callbackParams] !== undefined) fail(library, 'unsupported_callback_params', `${callbackParams} cannot be preserved`);
  }
  if (vars.overwrite === false) fail(library, 'unsupported_overwrite', 'The runtime currently owns each target/property with replace semantics');
  if (vars.repeat === -1 || vars.repeat === Infinity) fail(library, 'unsupported_infinite_loop', 'Infinite repeats are not supported');
  const repeats = Math.max(0, Math.floor(vars.repeat ?? 0));
  if ((vars.repeatDelay ?? 0) !== 0 && repeats > 0) fail(library, 'unsupported_loop_delay', 'repeatDelay is not represented by the current IR');
  const direction: RmlAnimationDirection = vars.yoyo
    ? (vars.reversed ? 'alternate-reverse' : 'alternate')
    : (vars.reversed ? 'reverse' : 'normal');
  const requestedDuration = vars.duration === undefined
    ? undefined
    : finiteNumber(library, 'duration', vars.duration, 0.5, false);
  const duration = requestedDuration ?? 0.5;
  const delay = finiteNumber(library, 'delay', vars.delay, 0);
  const options: RmlAnimationOptions = {
    duration,
    delay,
    iterations: repeats + 1,
    direction, fill: 'both', composite: 'replace', fallback: 'js-batched',
  };
  const toProperties = Object.fromEntries(Object.entries(vars).filter(([name]) => !gsapMetadata.has(name)));
  if (vars.autoAlpha !== undefined) {
    if (typeof vars.autoAlpha !== 'number' || !Number.isFinite(vars.autoAlpha) || vars.autoAlpha < 0 || vars.autoAlpha > 1)
      fail(library, 'invalid_auto_alpha', 'autoAlpha must be a number within 0..1');
    toProperties.opacity = vars.autoAlpha;
    toProperties.visibility = vars.autoAlpha === 0 ? 'hidden' : 'visible';
    if (from?.autoAlpha !== undefined) {
      const source = Number(from.autoAlpha);
      if (!Number.isFinite(source) || source < 0 || source > 1)
        fail(library, 'invalid_auto_alpha', 'from autoAlpha must be a number within 0..1');
      from = { ...from, opacity: source, visibility: source === 0 ? 'hidden' : 'visible' };
    }
  }
  const stagger = normalizeGsapStagger(library, vars.stagger);
  if (vars.keyframes !== undefined) {
    if (from) fail(library, 'unsupported_keyframes_from_to', 'GSAP fromTo cannot be combined with keyframes');
    if (Object.keys(toProperties).length)
      fail(library, 'unsupported_mixed_keyframes', 'Direct properties cannot be combined with the keyframes option');
    const compiled = Array.isArray(vars.keyframes)
      ? gsapSequentialKeyframeTracks(library, vars.keyframes, requestedDuration, vars.ease)
      : gsapKeyframeTracks(library, vars.keyframes, vars.ease);
    return {
      library, targets, tracks: compiled.tracks,
      options: { ...options, duration: compiled.duration ?? options.duration },
      defaultEasing: compiled.defaultEasing, stagger,
    };
  }
  if (from) {
    for (const name of Object.keys(toProperties)) {
      if (!(name in from)) fail(library, 'missing_from_value', `fromTo is missing ${name}`);
      toProperties[name] = [from[name], toProperties[name]];
    }
  }
  const easing = normalizePowerEasing(library, vars.ease, 'power1.out');
  return {
    library,
    targets,
    tracks: propertyTracks(library, toProperties),
    options,
    defaultEasing: easing,
    stagger,
  };
}

function gsapPlans(targets: RmlAnimationTarget, from: Record<string, unknown> | undefined, vars: GsapVars): RmlAnimationCompilePlan[] {
  return compileAnimationSourceRequest(gsapSourceRequest(targets, from, vars));
}

function parseGsapTimelinePosition(
  library: string,
  position: GsapTimelinePosition | undefined,
  hasPrevious: boolean,
  labels: ReadonlySet<string>,
): RmlAnimationTimelinePositionExpression {
  if (position === undefined) return { anchor: 'timeline-end' };
  if (typeof position === 'number') {
    if (!Number.isFinite(position) || position < 0)
      return fail(library, 'invalid_timeline_position', 'Timeline positions must be non-negative finite seconds');
    return { anchor: 'absolute', value: normalizedTimelineNumber(position) };
  }
  if (typeof position !== 'string')
    return fail(library, 'invalid_timeline_position', 'Timeline position must be seconds or a supported position string');
  const text = position.trim();
  if (labels.has(text)) return { anchor: 'label', label: text };
  const relative = text.match(/^([+-])=\s*(\d+(?:\.\d+)?)$/);
  if (relative)
    return { anchor: 'timeline-end', offset: (relative[1] === '+' ? 1 : -1) * Number(relative[2]) };
  const anchor = text.match(/^([<>])(?:([+-])=\s*(\d+(?:\.\d+)?))?$/);
  if (anchor) {
    if (!hasPrevious) return fail(library, 'missing_timeline_anchor', `${anchor[1]} requires a previous tween`);
    return {
      anchor: anchor[1] === '<' ? 'previous-start' : 'previous-end',
      offset: (anchor[2] === '-' ? -1 : 1) * Number(anchor[3] ?? 0),
    };
  }
  const labelRelative = text.match(/^([A-Za-z_$][\w$.-]*)([+-])=\s*(\d+(?:\.\d+)?)$/);
  if (labelRelative) {
    if (!labels.has(labelRelative[1])) return fail(library, 'unknown_timeline_label', `Unknown timeline label: ${labelRelative[1]}`);
    return {
      anchor: 'label', label: labelRelative[1],
      offset: (labelRelative[2] === '+' ? 1 : -1) * Number(labelRelative[3]),
    };
  }
  return fail(library, 'unknown_timeline_label', `Unknown timeline label or position syntax: ${text}`);
}

export function adaptGsapTo(targets: RmlAnimationTarget, vars: GsapVars): RmlAnimationGroup {
  const library = 'gsap@3.15.0';
  const group = runPlans(library, gsapPlans(targets, undefined, vars), vars.onComplete, vars.onInterrupt);
  if (vars.paused === true) group.pause();
  return group;
}

export function adaptGsapFromTo(
  targets: RmlAnimationTarget,
  fromVars: Record<string, unknown>,
  toVars: GsapVars,
): RmlAnimationGroup {
  const library = 'gsap@3.15.0';
  const group = runPlans(library, gsapPlans(targets, fromVars, toVars), toVars.onComplete, toVars.onInterrupt);
  if (toVars.paused === true) group.pause();
  return group;
}

export function adaptGsapTimeline(
  entries: readonly (GsapTimelineTween | GsapTimelineSet | GsapTimelineLabel)[],
  options: GsapTimelineOptions = {},
): RmlAnimationGroup {
  const library = 'gsap@3.15.0';
  if (!entries.length) fail(library, 'empty_timeline', 'Timeline requires at least one entry');
  const labels = new Set<string>();
  const timelineEntries: RmlAnimationTimelineEntry[] = [];
  let hasPrevious = false;
  for (const entry of entries) {
    if ('label' in entry) {
      const label = entry.label.trim();
      if (!/^[A-Za-z_$][\w$.-]*$/.test(label))
        fail(library, 'invalid_timeline_label', `Invalid timeline label: ${entry.label}`);
      if (labels.has(label)) fail(library, 'duplicate_timeline_label', `Duplicate timeline label: ${label}`);
      const position = parseGsapTimelinePosition(library, entry.position, hasPrevious, labels);
      labels.add(label);
      timelineEntries.push({ label, position });
      continue;
    }
    if (!('set' in entry) && (entry.vars.onComplete !== undefined || entry.vars.onInterrupt !== undefined))
      fail(library, 'unsupported_timeline_child_callback', 'Timeline child completion callbacks cannot survive track merging');
    if (!('set' in entry) && entry.vars.paused === true)
      fail(library, 'unsupported_timeline_child_pause', 'Pause the compiled timeline group instead of an individual child');
    const source = 'set' in entry
      ? instantSourceRequest(library, entry.targets, entry.set, gsapMetadata)
      : gsapSourceRequest(entry.targets, undefined, entry.vars);
    const position = parseGsapTimelinePosition(library, entry.position, hasPrevious, labels);
    timelineEntries.push({ position, source, overlap: 'layered-replace' });
    hasPrevious = true;
  }
  if (!hasPrevious) fail(library, 'empty_timeline', 'Timeline contains labels but no animation steps');
  const group = runPlans(library, compileAnimationTimelineSteps(library, timelineEntries), options.onComplete, options.onInterrupt);
  if (options.paused === true) group.pause();
  return group;
}
