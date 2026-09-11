import postcss from 'postcss';

const motionFields = {
  animation: new Map([
    ['animation-duration', 'duration'], ['animation-timing-function', 'timing'],
    ['animation-delay', 'delay'], ['animation-iteration-count', 'iteration'],
    ['animation-direction', 'direction'], ['animation-play-state', 'playState'],
    ['animation-name', 'name'],
  ]),
  transition: new Map([
    ['transition-duration', 'duration'], ['transition-timing-function', 'timing'],
    ['transition-delay', 'delay'], ['transition-property', 'property'],
  ]),
};

const timingFunctions = new Map([
  ['linear', 'linear-in-out'], ['ease', 'cubic-out'], ['ease-in', 'cubic-in'],
  ['ease-out', 'cubic-out'], ['ease-in-out', 'cubic-in-out'],
]);
const supportedVendorProperties = new Set([
  ...motionFields.animation.keys(), ...motionFields.transition.keys(),
  'animation', 'transition', 'transform', 'transform-origin',
]);

function diagnostic(decl, severity, code, message) {
  return { severity, code, message, source: decl.source?.input?.file ?? '<css>', line: decl.source?.start?.line ?? 0, column: decl.source?.start?.column ?? 0 };
}

function isInsideKeyframes(rule) {
  for (let node = rule.parent; node; node = node.parent) {
    if (node.type === 'atrule' && /keyframes$/i.test(node.name)) return true;
  }
  return false;
}

function normalizeVendorDeclarations(rule, diagnostics) {
  const declarations = rule.nodes?.filter((node) => node.type === 'decl') ?? [];
  const standardProperties = new Set(declarations.map((decl) => decl.prop.toLowerCase()));
  for (const decl of declarations) {
    const match = decl.prop.toLowerCase().match(/^-(webkit|moz|o)-(.+)$/);
    if (!match) continue;
    const standard = match[2];
    if (standardProperties.has(standard)) decl.remove();
    else if (supportedVendorProperties.has(standard)) {
      decl.prop = standard;
      standardProperties.add(standard);
    } else {
      diagnostics.push(diagnostic(decl, 'warning', 'vendor-property-dropped', `Dropped unsupported vendor property ${decl.prop}.`));
      decl.remove();
    }
  }
}

function normalizeBrowserOnlyTransforms(rule, diagnostics) {
  for (const decl of rule.nodes?.filter((node) => node.type === 'decl' && node.prop.toLowerCase() === 'transform') ?? []) {
    if (/^perspective\(\s*1px\s*\)\s+translatez\(\s*0(?:px)?\s*\)$/i.test(decl.value.trim())) {
      diagnostics.push(diagnostic(decl, 'warning', 'gpu-promotion-transform-normalized', 'Replaced the browser-only GPU promotion transform with scale(1) so RmlUi can interpolate it.'));
      decl.value = 'scale(1)';
    }
  }
}

function normalizeBorderStyleTokens(rule, diagnostics) {
  const borderProperties = /^(border|border-(top|right|bottom|left))$/i;
  for (const decl of rule.nodes?.filter((node) => node.type === 'decl' && borderProperties.test(node.prop)) ?? []) {
    if (/\b(dashed|dotted|double|groove|ridge|inset|outset)\b/i.test(decl.value)) {
      diagnostics.push(diagnostic(decl, 'error', 'unsupported-border-style', `${decl.prop}: ${decl.value} uses a border style without an RmlUi equivalent.`));
      continue;
    }
    decl.value = decl.value.replace(/\bsolid\b/ig, ' ').replace(/\s+/g, ' ').trim();
  }
}

function lowerUnrealMaterial(rule, diagnostics) {
  const declarations = rule.nodes?.filter((node) => node.type === 'decl') ?? [];
  const binding = declarations.find((decl) => decl.prop.toLowerCase() === '-rmlui-material');
  const slot = declarations.find((decl) => decl.prop.toLowerCase() === '-rmlui-material-slot');
  if (!binding) {
    slot?.remove();
    return;
  }
  const alias = binding.value.trim().replace(/^['"]|['"]$/g, '');
  const slotName = slot?.value.trim().toLowerCase() || 'background';
  if (!/^[-_.a-zA-Z0-9]+$/.test(alias)) {
    diagnostics.push(diagnostic(binding, 'error', 'invalid-unreal-material-alias', '-rmlui-material must be a registered alias, not an Unreal asset path or expression.'));
  } else if (slotName === 'foreground') {
    diagnostics.push(diagnostic(slot ?? binding, 'error', 'unsupported-unreal-material-slot', 'The foreground material slot requires a render stage after element content and is not supported yet.'));
  } else if (!['background', 'border'].includes(slotName)) {
    diagnostics.push(diagnostic(slot ?? binding, 'error', 'invalid-unreal-material-slot', '-rmlui-material-slot must be background or border.'));
  } else {
    const decoratorName = slotName === 'border' ? 'ue-material-border' : 'ue-material';
    rule.append({ prop: 'decorator', value: `${decoratorName}(${alias})`, important: binding.important });
  }
  binding.remove();
  slot?.remove();
}

function singleValue(decl, diagnostics) {
  if (postcss.list.comma(decl.value).length > 1) {
    diagnostics.push(diagnostic(decl, 'error', 'multiple-motion-values', `${decl.prop} lists are not supported by WebCompat v1.`));
    return null;
  }
  return decl.value.trim();
}

function convertTiming(decl, diagnostics) {
  const value = singleValue(decl, diagnostics);
  if (value === null) return null;
  const lower = value.toLowerCase();
  if (timingFunctions.has(lower)) return timingFunctions.get(lower);
  if (/^(back|bounce|circular|cubic|elastic|exponential|linear|quadratic|quartic|quintic|sine)-(in|out|in-out)$/.test(lower)) return lower;
  diagnostics.push(diagnostic(decl, 'error', 'unsupported-timing-function', `${decl.prop}: ${decl.value} has no RmlUi 6.3 equivalent.`));
  return null;
}

function convertValue(kind, field, decl, diagnostics) {
  if (field === 'timing') return convertTiming(decl, diagnostics);
  const value = singleValue(decl, diagnostics);
  if (value === null) return null;
  const lower = value.toLowerCase();
  if (kind === 'animation' && field === 'direction') {
    if (lower === 'normal') return '';
    if (lower === 'alternate') return 'alternate';
    diagnostics.push(diagnostic(decl, 'error', 'unsupported-animation-direction', `${decl.prop}: ${decl.value} has no RmlUi 6.3 equivalent.`));
    return null;
  }
  if (kind === 'animation' && field === 'playState') {
    if (lower === 'running') return '';
    if (lower === 'paused') return 'paused';
    diagnostics.push(diagnostic(decl, 'error', 'unsupported-animation-play-state', `${decl.prop}: ${decl.value} is invalid.`));
    return null;
  }
  return value;
}

function splitSimpleSelectors(selector) {
  const selectors = postcss.list.comma(selector).map((item) => item.trim());
  return selectors.every((item) => /^[.#][-_a-zA-Z][\w-]*$/.test(item)) ? selectors : null;
}

function compatibleSimpleSelector(selector) {
  if (/^\.[-_a-zA-Z][\w-]*$/.test(selector) && /[A-Z]/.test(selector)) return `[class~="${selector.slice(1)}"]`;
  if (/^#[-_a-zA-Z][\w-]*$/.test(selector) && /[A-Z]/.test(selector)) return `[id="${selector.slice(1)}"]`;
  return selector;
}

function compatibleSelector(selector) {
  const selectors = splitSimpleSelectors(selector);
  return selectors ? selectors.map(compatibleSimpleSelector).join(', ') : selector;
}

function combineSelectors(provider, anchor) {
  const providers = splitSimpleSelectors(provider);
  const anchors = splitSimpleSelectors(anchor);
  if (!providers || !anchors) return null;
  return anchors.flatMap((right) => providers.map((left) => {
    const compatibleLeft = compatibleSimpleSelector(left);
    const compatibleRight = compatibleSimpleSelector(right);
    if (compatibleRight !== right) return compatibleRight;
    return left === right ? compatibleRight : `${compatibleLeft}${compatibleRight}`;
  })).join(', ');
}

function animationShorthand(values) {
  return [values.duration ?? '0.001s', values.timing ?? 'cubic-out', values.delay, values.iteration, values.direction, values.playState, values.name].filter(Boolean).join(' ');
}
function transitionShorthand(values) {
  return [values.property, values.duration ?? '0.001s', values.timing ?? 'cubic-out', values.delay].filter(Boolean).join(' ');
}

function emitComposedRules(root, records, kind, diagnostics) {
  const anchorField = kind === 'animation' ? 'name' : 'property';
  const providers = records.filter((record) => !record.values[anchorField] && Object.keys(record.values).length > 0);
  for (const anchor of records.filter((record) => record.values[anchorField])) {
    const shorthand = kind === 'animation' ? animationShorthand : transitionShorthand;
    if (anchor.values.duration || providers.length === 0) {
      const selector = compatibleSelector(anchor.rule.selector);
      if (selector === anchor.rule.selector) anchor.rule.append({ prop: kind, value: shorthand(anchor.values), important: anchor.important });
      else {
        const rule = postcss.rule({ selector });
        rule.append({ prop: kind, value: shorthand(anchor.values), important: anchor.important });
        anchor.rule.after(rule);
      }
      continue;
    }
    let composed = false;
    for (const provider of providers) {
      if (!provider.values.duration) continue;
      const selector = combineSelectors(provider.rule.selector, anchor.rule.selector);
      if (!selector) continue;
      if (selector === compatibleSelector(anchor.rule.selector) && selector !== anchor.rule.selector) {
        diagnostics.push(diagnostic(anchor.sourceDecl, 'warning', 'camel-case-selector-broadened', `RmlUi cannot reliably combine the camel-case selector ${anchor.rule.selector}; emitted ${selector} without the provider selector.`));
      }
      const rule = postcss.rule({ selector });
      rule.append({ prop: kind, value: shorthand({ ...provider.values, ...anchor.values }), important: provider.important || anchor.important });
      anchor.rule.after(rule);
      composed = true;
    }
    if (!composed) {
      anchor.rule.append({ prop: kind, value: shorthand(anchor.values), important: anchor.important });
      if (providers.some((provider) => provider.values.duration)) {
        diagnostics.push(diagnostic(anchor.sourceDecl, 'warning', 'selector-composition-skipped', `${kind} duration could not be composed with selector ${anchor.rule.selector}; the browser default was used.`));
      }
    }
  }
}

export function compileCss(source, options = {}) {
  const root = postcss.parse(source, { from: options.from });
  const diagnostics = [];
  const records = { animation: [], transition: [] };
  const standardKeyframes = new Set();
  root.walkAtRules(/^keyframes$/i, (rule) => standardKeyframes.add(rule.params.trim()));
  root.walkAtRules(/^-(webkit|moz|o)-keyframes$/i, (rule) => standardKeyframes.has(rule.params.trim()) ? rule.remove() : (rule.name = 'keyframes'));

  root.walkRules((rule) => {
    normalizeVendorDeclarations(rule, diagnostics);
    normalizeBrowserOnlyTransforms(rule, diagnostics);
    normalizeBorderStyleTokens(rule, diagnostics);
    lowerUnrealMaterial(rule, diagnostics);
    if (isInsideKeyframes(rule)) return;
    const declarations = [...(rule.nodes?.filter((node) => node.type === 'decl') ?? [])];
    for (const kind of ['animation', 'transition']) {
      const fields = motionFields[kind];
      const matching = declarations.filter((decl) => fields.has(decl.prop.toLowerCase()));
      if (matching.length === 0) continue;
      if (declarations.some((decl) => decl.prop.toLowerCase() === kind)) {
        diagnostics.push(diagnostic(matching[0], 'error', `mixed-${kind}-syntax`, `Do not mix ${kind} shorthand and longhands in one rule in WebCompat v1.`));
        continue;
      }
      const record = { rule, values: {}, important: false, sourceDecl: matching[0] };
      for (const decl of matching) {
        const value = convertValue(kind, fields.get(decl.prop.toLowerCase()), decl, diagnostics);
        if (value !== null) record.values[fields.get(decl.prop.toLowerCase())] = value;
        record.important ||= decl.important;
        decl.remove();
      }
      records[kind].push(record);
    }
    for (const decl of [...(rule.nodes?.filter((node) => node.type === 'decl') ?? [])]) {
      if (decl.prop.toLowerCase() === 'animation-fill-mode') {
        diagnostics.push(diagnostic(decl, 'warning', 'animation-fill-mode-dropped', 'RmlUi 6.3 has no animation-fill-mode; the declaration was dropped.'));
        decl.remove();
      }
    }
  });
  emitComposedRules(root, records.animation, 'animation', diagnostics);
  emitComposedRules(root, records.transition, 'transition', diagnostics);
  const css = root.toString();
  return { css, diagnostics, changed: css !== source };
}
