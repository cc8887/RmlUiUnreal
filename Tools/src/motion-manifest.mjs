import postcss from 'postcss';

const propertyIds = new Map([
  ['opacity', 1], ['transform', 2], ['left', 3], ['top', 4], ['right', 5],
  ['bottom', 6], ['width', 7], ['height', 8], ['visibility', 9], ['color', 10],
  ['background-color', 11], ['border-color', 12], ['image-color', 13],
]);

function parseTime(value) {
  const match = String(value ?? '').trim().toLowerCase().match(/^([+-]?(?:\d+(?:\.\d*)?|\.\d+))(ms|s)$/);
  if (!match) return undefined;
  const seconds = Number(match[1]) * (match[2] === 'ms' ? 0.001 : 1);
  return Number.isFinite(seconds) ? seconds : undefined;
}

function parseEasing(source = 'ease') {
  const text = source.trim().toLowerCase();
  const presets = {
    linear: [0, 0, 0, 1, 1], ease: [1, 0.25, 0.1, 0.25, 1],
    'ease-in': [1, 0.42, 0, 1, 1], 'ease-out': [1, 0, 0, 0.58, 1],
    'ease-in-out': [1, 0.42, 0, 0.58, 1], 'step-start': [2, 1, 1, 0, 0],
    'step-end': [2, 1, 0, 0, 0],
  };
  if (presets[text]) return presets[text];
  const cubic = text.match(/^cubic-bezier\(\s*([^,]+),\s*([^,]+),\s*([^,]+),\s*([^\)]+)\)$/);
  if (cubic) {
    const points = cubic.slice(1).map(Number);
    if (points.every(Number.isFinite) && points[0] >= 0 && points[0] <= 1 && points[2] >= 0 && points[2] <= 1)
      return [1, ...points];
    return undefined;
  }
  const steps = text.match(/^steps\(\s*(\d+)\s*(?:,\s*(start|end|jump-start|jump-end|jump-none|jump-both)\s*)?\)$/);
  if (!steps) return undefined;
  const count = Number(steps[1]);
  const position = { end: 0, 'jump-end': 0, start: 1, 'jump-start': 1, 'jump-none': 2, 'jump-both': 3 }[steps[2] ?? 'end'];
  return count >= 1 && !(position === 2 && count < 2) ? [2, count, position, 0, 0] : undefined;
}

function parseColor(value) {
  const text = value.trim().toLowerCase();
  if (text === 'transparent') return [0, 0, 0, 0];
  const hex = text.match(/^#([0-9a-f]{3,4}|[0-9a-f]{6}|[0-9a-f]{8})$/)?.[1];
  if (hex) {
    const expanded = hex.length <= 4 ? [...hex].map((char) => char + char).join('') : hex;
    return [0, 2, 4, 6].map((offset, index) => index === 3 && expanded.length === 6
      ? 1 : Number.parseInt(expanded.slice(offset, offset + 2), 16) / 255);
  }
  const rgb = text.match(/^rgba?\(\s*([^,]+),\s*([^,]+),\s*([^,\)]+)(?:,\s*([^\)]+))?\)$/);
  if (!rgb) return undefined;
  const result = [Number(rgb[1]) / 255, Number(rgb[2]) / 255, Number(rgb[3]) / 255,
    rgb[4] === undefined ? 1 : Number(rgb[4])];
  return result.every((channel) => Number.isFinite(channel) && channel >= 0 && channel <= 1) ? result : undefined;
}

function parseTransform(value) {
  const text = value.trim().toLowerCase();
  if (text === 'none') return { values: [0, 0, 1, 1, 0, 0, 0], primitive: 0 };
  const values = [0, 0, 1, 1, 0, 0, 0];
  const finite = (source, suffix = '') => {
    let number = source.trim();
    if (suffix && !number.endsWith(suffix)) return undefined;
    if (suffix) number = number.slice(0, -suffix.length).trim();
    const parsed = Number(number);
    return number && Number.isFinite(parsed) ? parsed : undefined;
  };
  let primitive = 0;
  let cursor = 0;
  const pattern = /\s*(translate|translatex|translatey|scale|scalex|scaley|rotate|skewx|skewy|skew|matrix)\(([^\(\)]*)\)/gy;
  while (cursor < text.length) {
    pattern.lastIndex = cursor;
    const match = pattern.exec(text);
    if (!match || match.index !== cursor) return undefined;
    const parts = postcss.list.comma(match[2]).map((part) => part.trim());
    const name = match[1];
    const bit = name.startsWith('translate') ? 1 : name.startsWith('scale') ? 2 : name === 'rotate' ? 4 :
      name === 'skewx' ? 8 : name === 'skewy' ? 16 : name === 'skew' ? 24 : 31;
    if (primitive & bit) return undefined;
    if (name === 'translate' && (parts.length === 1 || parts.length === 2)) {
      const x = finite(parts[0], 'px'); const y = finite(parts[1] ?? '0px', 'px');
      if (x === undefined || y === undefined) return undefined;
      values[0] = x; values[1] = y;
    } else if ((name === 'translatex' || name === 'translatey') && parts.length === 1) {
      const component = finite(parts[0], 'px'); if (component === undefined) return undefined;
      values[name === 'translatex' ? 0 : 1] = component;
    } else if (name === 'scale' && (parts.length === 1 || parts.length === 2)) {
      const x = finite(parts[0]); const y = finite(parts[1] ?? parts[0]);
      if (x === undefined || y === undefined) return undefined;
      values[2] = x; values[3] = y;
    } else if ((name === 'scalex' || name === 'scaley') && parts.length === 1) {
      const component = finite(parts[0]); if (component === undefined) return undefined;
      values[name === 'scalex' ? 2 : 3] = component;
    } else if (name === 'rotate' && parts.length === 1) {
      const angle = finite(parts[0], 'deg'); if (angle === undefined) return undefined; values[4] = angle;
    } else if ((name === 'skewx' || name === 'skewy' || name === 'skew') && (parts.length === 1 || (name === 'skew' && parts.length === 2))) {
      const x = finite(parts[0], 'deg'); const y = finite(parts[1] ?? '0deg', 'deg');
      if (x === undefined || y === undefined) return undefined;
      if (name === 'skewy') values[6] = x; else { values[5] = x; values[6] = y; }
    } else if (name === 'matrix' && parts.length === 6) {
      const matrix = parts.map((part) => finite(part));
      if (matrix.some((part) => part === undefined)) return undefined;
      const [a, b, c, d, tx, ty] = matrix;
      const scaleX = Math.hypot(a, b); if (scaleX <= Number.EPSILON) return undefined;
      values[0] = tx; values[1] = ty; values[2] = scaleX; values[3] = (a * d - b * c) / scaleX;
      values[4] = Math.atan2(b, a) * 180 / Math.PI;
      values[5] = Math.atan((a * c + b * d) / (scaleX * scaleX)) * 180 / Math.PI;
    } else return undefined;
    primitive |= bit;
    cursor = pattern.lastIndex;
  }
  return primitive ? { values, primitive } : undefined;
}

function parseValue(property, value) {
  if (property === 'opacity') {
    const parsed = Number(value); return Number.isFinite(parsed) && parsed >= 0 && parsed <= 1 ? [parsed] : undefined;
  }
  if (property === 'transform') return parseTransform(value);
  if (property === 'visibility') {
    const text = value.trim().toLowerCase(); return text === 'visible' ? [1] : text === 'hidden' ? [0] : undefined;
  }
  if (propertyIds.get(property) >= 10) return parseColor(value);
  const match = value.trim().match(/^([+-]?(?:\d+(?:\.\d*)?|\.\d+))px$/i);
  if (!match) return undefined;
  const parsed = Number(match[1]);
  return Number.isFinite(parsed) && (!(property === 'width' || property === 'height') || parsed >= 0) ? [parsed] : undefined;
}

function frameOffsets(selector) {
  const offsets = postcss.list.comma(selector).map((part) => part.trim().toLowerCase()).map((part) =>
    part === 'from' ? 0 : part === 'to' ? 1 : /^\d+(?:\.\d+)?%$/.test(part) ? Number(part.slice(0, -1)) / 100 : NaN);
  return offsets.every((offset) => Number.isFinite(offset) && offset >= 0 && offset <= 1) ? offsets : undefined;
}

function compileKeyframes(atRule, defaultEasing) {
  const byProperty = new Map();
  for (const rule of atRule.nodes?.filter((node) => node.type === 'rule') ?? []) {
    const offsets = frameOffsets(rule.selector);
    if (!offsets) return { error: `Invalid keyframe selector '${rule.selector}'.` };
    const frameEasing = rule.nodes?.find((node) => node.type === 'decl' && node.prop.toLowerCase() === 'animation-timing-function')?.value ?? defaultEasing;
    const easing = parseEasing(frameEasing);
    if (!easing) return { error: `Unsupported keyframe timing function '${frameEasing}'.` };
    for (const decl of rule.nodes?.filter((node) => node.type === 'decl' && node.prop.toLowerCase() !== 'animation-timing-function') ?? []) {
      const property = decl.prop.toLowerCase();
      const propertyId = propertyIds.get(property);
      if (!propertyId) return { error: `Property '${decl.prop}' cannot be represented by the native animation IR.` };
      const parsed = parseValue(property, decl.value);
      if (!parsed) return { error: `Value '${decl.value}' for '${decl.prop}' cannot be represented by the native animation IR.` };
      for (const offset of offsets) byProperty.set(property, [...(byProperty.get(property) ?? []), {
        offset, values: Array.isArray(parsed) ? parsed : parsed.values, easing,
        primitive: Array.isArray(parsed) ? 0 : parsed.primitive ?? 0,
      }]);
    }
  }
  const tracks = [];
  for (const [property, frames] of byProperty) {
    frames.sort((a, b) => a.offset - b.offset);
    if (frames.length < 2 || frames[0].offset !== 0 || frames.at(-1).offset !== 1)
      return { error: `Property '${property}' must define explicit 0% and 100% keyframes in native CSS animations.` };
    if (property === 'transform') {
      const primitives = new Set(frames.map((frame) => frame.primitive).filter(Boolean));
      if (primitives.size > 1) return { error: 'Transform keyframes must use the same primitive list.' };
    }
    tracks.push({ property, propertyId: propertyIds.get(property), keyframes: frames.map(({ offset, values, easing }) => ({ offset, values, easing })) });
  }
  return tracks.length ? { tracks } : { error: 'The keyframes rule has no animatable declarations.' };
}

function splitSimpleSelectors(selector) {
  const selectors = postcss.list.comma(selector).map((item) => item.trim());
  return selectors.every((item) => /^[.#][-_a-zA-Z][\w-]*$/.test(item)) ? selectors : null;
}

function compatible(selector) {
  if (/^\.[-_a-zA-Z][\w-]*$/.test(selector) && /[A-Z]/.test(selector)) return `[class~="${selector.slice(1)}"]`;
  if (/^#[-_a-zA-Z][\w-]*$/.test(selector) && /[A-Z]/.test(selector)) return `[id="${selector.slice(1)}"]`;
  return selector;
}

function combinedSelectors(provider, anchor) {
  const left = splitSimpleSelectors(provider); const right = splitSimpleSelectors(anchor);
  if (!left || !right) return undefined;
  return right.flatMap((r) => left.map((l) => l === r ? compatible(r) : `${compatible(l)}${compatible(r)}`)).join(', ');
}

function unsupportedActivationSelector(selector) {
  let bracketDepth = 0;
  let quote = '';
  for (let i = 0; i < selector.length; i++) {
    const char = selector[i];
    if (quote) {
      if (char === '\\') i++;
      else if (char === quote) quote = '';
    } else if (char === '"' || char === "'") quote = char;
    else if (char === '[') bracketDepth++;
    else if (char === ']') bracketDepth--;
    else if (!bracketDepth && char === ':') return 'Pseudo-state selectors require input/state invalidation that the native activation manager does not provide.';
    else if (!bracketDepth && (char === '+' || char === '~')) return 'Sibling selectors require sibling invalidation that the native activation manager does not provide.';
  }
  return undefined;
}

export function extractMotionManifest(root, records, diagnostics, makeDiagnostic) {
  const keyframes = new Map();
  root.walkAtRules(/^keyframes$/i, (rule) => keyframes.set(rule.params.trim(), rule));
  const providers = records.filter((record) => !record.rawValues.name && Object.keys(record.rawValues).length);
  const rules = [];
  const playStates = [];
  const consumedKeyframes = new Set();
  for (const record of records) {
    if (!record.rawValues.playState) continue;
    const selectorError = unsupportedActivationSelector(record.rule.selector);
    if (selectorError) {
      diagnostics.push(makeDiagnostic(record.sourceDecl, 'error', 'unsupported-native-animation-selector', selectorError));
      continue;
    }
    const state = record.rawValues.playState.trim().toLowerCase();
    if (state === 'running' || state === 'paused')
      playStates.push({ selector: record.rule.selector, paused: state === 'paused' });
  }
  for (const anchor of records.filter((record) => record.rawValues.name)) {
    const candidates = anchor.rawValues.duration ? [{ selector: anchor.rule.selector, values: anchor.rawValues, provider: anchor }] :
      providers.filter((provider) => provider.rawValues.duration).map((provider) => ({
        selector: combinedSelectors(provider.rule.selector, anchor.rule.selector),
        values: { ...provider.rawValues, ...anchor.rawValues },
        provider,
      }));
    for (const candidate of candidates) {
      if (!candidate.selector) continue;
      const source = keyframes.get(candidate.values.name);
      if (!source) {
        diagnostics.push(makeDiagnostic(anchor.sourceDecl, 'warning', 'native-keyframes-not-in-source',
          `@keyframes ${candidate.values.name} is not in this CSS source; the rule uses the RmlUi RCSS fallback instead of the native animation IR.`));
        continue;
      }
      const selectorError = unsupportedActivationSelector(candidate.selector);
      if (selectorError) {
        diagnostics.push(makeDiagnostic(anchor.sourceDecl, 'error', 'unsupported-native-animation-selector', selectorError));
        continue;
      }
      const duration = parseTime(candidate.values.duration ?? '0s');
      const delay = parseTime(candidate.values.delay ?? '0s');
      // Runtime IR uses zero as the compact ECS representation of an unbounded count.
      const iterations = (candidate.values.iteration ?? '').trim().toLowerCase() === 'infinite'
        ? 0 : Number(candidate.values.iteration ?? 1);
      const direction = ['normal', 'reverse', 'alternate', 'alternate-reverse'].indexOf((candidate.values.direction ?? 'normal').toLowerCase());
      const fill = ['none', 'forwards', 'backwards', 'both'].indexOf((candidate.values.fill ?? 'none').toLowerCase());
      const compiled = compileKeyframes(source, candidate.values.timing ?? 'ease');
      if (duration === undefined || duration < 0 || delay === undefined || !Number.isInteger(iterations) || iterations < 0 || direction < 0 || fill < 0 || compiled.error) {
        const reason = compiled.error ?? 'Duration, delay, iteration count, direction, or fill mode is invalid.';
        diagnostics.push(makeDiagnostic(anchor.sourceDecl, 'error', 'unsupported-native-css-animation', reason));
        continue;
      }
      rules.push({ selector: candidate.selector, name: candidate.values.name, duration, delay, iterations, direction, fill,
        paused: (candidate.values.playState ?? 'running').toLowerCase() === 'paused', tracks: compiled.tracks });
      consumedKeyframes.add(source);
      anchor.native = true;
      candidate.provider.native = true;
    }
  }
  for (const rule of consumedKeyframes) rule.remove();
  return playStates.length ? { schemaVersion: 1, rules, playStates } : { schemaVersion: 1, rules };
}
