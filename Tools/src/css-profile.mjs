import postcss from 'postcss';
import { capabilityCatalog, createCapabilities, resolveProfile } from './capabilities.mjs';

const properties = new Set(('display position top left right bottom width height min-width min-height max-width max-height box-sizing overflow overflow-x overflow-y ' +
  'margin margin-top margin-right margin-bottom margin-left padding padding-top padding-right padding-bottom padding-left ' +
  'color background background-color caret-color image-color font-family font-size font-weight font-style font-kerning letter-spacing line-height text-align text-decoration text-transform text-overflow white-space word-break vertical-align ' +
  'border border-width border-color border-style border-radius border-top border-bottom border-left border-right border-left-width border-right-width border-top-width border-bottom-width border-top-color border-right-color border-bottom-color border-left-color border-top-left-radius border-top-right-radius border-bottom-left-radius border-bottom-right-radius ' +
  'opacity cursor drag visibility z-index pointer-events tab-index focus nav-up nav-right nav-down nav-left scrollbar-margin overscroll-behavior clip float clear ' +
  'flex flex-grow flex-shrink flex-basis flex-direction flex-wrap align-items align-self align-content justify-content justify-items justify-self order ' +
  'gap row-gap column-gap grid-template-columns grid-template-rows grid-template-areas grid-area grid-row grid-column grid-row-start grid-row-end grid-column-start grid-column-end grid-auto-flow grid-auto-rows grid-auto-columns ' +
  'decorator mask-image filter backdrop-filter box-shadow font-effect fill-image transform transform-origin transform-origin-x transform-origin-y transform-origin-z perspective perspective-origin ' +
  'animation transition -rmlui-language -rmlui-direction').split(/\s+/));
const enums = {
  display: ['none', 'block', 'inline', 'inline-block', 'flex', 'inline-flex', 'grid', 'inline-grid', 'table', 'table-row', 'table-row-group', 'table-column', 'table-column-group', 'table-cell'],
  position: ['static', 'relative', 'absolute', 'fixed'],
  'box-sizing': ['border-box', 'content-box'],
  'pointer-events': ['none', 'auto'], visibility: ['visible', 'hidden'],
  'overflow': ['visible', 'hidden', 'auto', 'scroll'], 'overflow-x': ['visible', 'hidden', 'auto', 'scroll'], 'overflow-y': ['visible', 'hidden', 'auto', 'scroll'],
  'flex-direction': ['row', 'column', 'row-reverse', 'column-reverse'], 'flex-wrap': ['nowrap', 'wrap', 'wrap-reverse'],
  'white-space': ['normal', 'pre', 'nowrap', 'pre-wrap', 'pre-line'], 'word-break': ['normal', 'break-all', 'break-word'],
  'font-style': ['normal', 'italic'], 'text-align': ['left', 'right', 'center', 'justify'],
  'focus': ['none', 'auto'], 'tab-index': ['none', 'auto'],
};
const functions = new Set(('var rgb rgba hsl hsla lab lch oklab oklch minmax repeat fit-content translate translatex translatey translatez translate3d scale scalex scaley scalez scale3d rotate rotatex rotatey rotatez rotate3d skew skewx skewy matrix matrix3d perspective ' +
  'blur opacity brightness contrast invert grayscale sepia saturate hue-rotate drop-shadow horizontal-gradient vertical-gradient linear-gradient radial-gradient conic-gradient repeating-linear-gradient repeating-radial-gradient repeating-conic-gradient ' +
  'image ninepatch tiled-box tiled-horizontal tiled-vertical ue-material ue-material-border shader url shadow outline glow').split(/\s+/));
const numeric = /^[-+]?(?:\d*\.)?\d+(?:e[-+]?\d+)?$/i;
const channels = /^[-+]?(?:\d*\.)?\d+%?$/;
const length = /^[-+]?(?:\d*\.)?\d+(?:px|dp|em|rem|vw|vh|in|cm|mm|pt|pc|%)$/i;
const namedColors = new Set('black silver gray grey white maroon red orange purple fuchsia green lime olive yellow navy blue teal aqua transparent'.split(' '));
const transform3d = /\b(?:perspective|matrix3d|translate3d|translatez|scale3d|scalez|rotate3d|rotatex|rotatey)\s*\(/i;
const shader = /\b(?:shader|(?:repeating-)?(?:linear|radial|conic)-gradient)\s*\(/i;

export function cssDiagnostic(node, severity, code, message, classification = severity === 'error' ? 'rejected' : 'exact') {
  let rule = node;
  while (rule && rule.type !== 'rule') rule = rule.parent;
  return {
    severity, classification, code, message,
    source: node.source?.input?.file ?? node.source?.input?.from ?? '<css>',
    line: node.source?.start?.line ?? 0, column: node.source?.start?.column ?? 0,
    selector: rule?.selector ?? '', property: node.prop ?? '', value: node.value ?? '',
  };
}

// Split only outside strings/functions. This is also used for var() fallbacks containing colors.
function splitTopLevel(value, separator) {
  const values = []; let start = 0, depth = 0, quote = '';
  for (let index = 0; index < value.length; ++index) {
    const character = value[index];
    if (quote) { if (character === quote && value[index - 1] !== '\\') quote = ''; continue; }
    if (character === '"' || character === "'") quote = character;
    else if (character === '(') ++depth;
    else if (character === ')') --depth;
    else if (!depth && separator.test(character)) { values.push(value.slice(start, index).trim()); start = index + 1; }
  }
  values.push(value.slice(start).trim());
  return values.filter(Boolean);
}

function rgbToHsl(values) {
  const [r, g, b] = values.map(value => Math.max(0, Math.min(1, parseFloat(value) / (value.endsWith('%') ? 100 : 255))));
  const max = Math.max(r, g, b), min = Math.min(r, g, b), delta = max - min, lightness = (max + min) / 2;
  const saturation = delta ? delta / (1 - Math.abs(2 * lightness - 1)) : 0;
  let hue = !delta ? 0 : max === r ? ((g - b) / delta) % 6 : max === g ? (b - r) / delta + 2 : (r - g) / delta + 4;
  hue = (hue * 60 + 360) % 360;
  return [Number(hue.toFixed(8)), `${Number((saturation * 100).toFixed(8))}%`, `${Number((lightness * 100).toFixed(8))}%`];
}

function normalizeColor(name, contents, decl, diagnostics) {
  const comma = splitTopLevel(contents, /,/), slash = splitTopLevel(contents, /\//);
  let components, alpha;
  if (comma.length > 1) { components = comma.slice(0, 3); alpha = comma[3]; }
  else { components = splitTopLevel(slash[0] ?? '', /\s/); alpha = slash[1]; }
  if (components.length !== 3 || comma.length > 4 || slash.length > 2 || (/a$/.test(name) && alpha === undefined)) {
    diagnostics.push(cssDiagnostic(decl, 'error', 'unsupported-color-components', `${name}() requires three explicit channels; use a complete color token with var(--color), rather than a token containing several channels.`));
    return `${name}(${contents})`;
  }
  const hsl = name.startsWith('hsl');
  if (hsl && (!/%$/.test(components[1]) || !/%$/.test(components[2]))) {
    diagnostics.push(cssDiagnostic(decl, 'error', 'invalid-hsl-components', 'HSL saturation and lightness must be explicit percentages.'));
    return `${name}(${contents})`;
  }
  if (alpha === undefined) return `${hsl ? 'hsl' : 'rgb'}(${components.join(',')})`;
  if (numeric.test(alpha)) {
    if (+alpha < 0 || +alpha > 1) diagnostics.push(cssDiagnostic(decl, 'error', 'invalid-css-alpha', 'CSS numeric alpha must be between 0 and 1; byte alpha is only accepted by the legacy Raw RCSS path.'));
    alpha = hsl ? alpha : `${Number((+alpha * 100).toFixed(6))}%`;
  } else if (/^\d*(?:\.\d+)?%$/.test(alpha)) {
    if (hsl) alpha = String(parseFloat(alpha) / 100);
  } else if (/^var\(/.test(alpha)) {
    // RmlUi RGB alpha is byte/percent, whereas HSL alpha is a 0..1 scalar.
    // Moving constant RGB channels to HSL preserves a live Tailwind alpha token.
    if (!hsl && components.every(value => channels.test(value))) {
      diagnostics.push(cssDiagnostic(decl, 'info', 'rgb-alpha-token-to-hsla', 'Converted constant RGB channels to HSLA so the inherited alpha token remains a live 0..1 value; native 8-bit color rounding may differ.', 'approximate'));
      return `hsla(${rgbToHsl(components).join(',')},${alpha})`;
    }
    if (!hsl) diagnostics.push(cssDiagnostic(decl, 'error', 'unsupported-dynamic-rgb-alpha', 'Dynamic RGB channels plus scalar alpha cannot be represented; use a complete color token or HSL with explicit channels.'));
  } else diagnostics.push(cssDiagnostic(decl, 'error', 'invalid-css-alpha', `Unsupported alpha expression ${alpha}.`));
  return `${hsl ? 'hsla' : 'rgba'}(${components.join(',')},${alpha})`;
}

function normalizeFunctions(value, decl, diagnostics) {
  let result = '', cursor = 0;
  const pattern = /([-a-zA-Z][\w-]*)\s*\(/g;
  for (let match; (match = pattern.exec(value));) {
    const start = match.index, open = pattern.lastIndex - 1;
    let depth = 1, end = open + 1, quote = '';
    for (; end < value.length && depth; ++end) {
      const ch = value[end];
      if (quote) { if (ch === quote && value[end - 1] !== '\\') quote = ''; continue; }
      if (ch === '"' || ch === "'") quote = ch;
      else if (ch === '(') ++depth;
      else if (ch === ')') --depth;
    }
    if (depth) { diagnostics.push(cssDiagnostic(decl, 'error', 'unclosed-css-function', `Unclosed ${match[1]}() expression.`)); return value; }
    const name = match[1].toLowerCase();
    const raw = value.slice(open + 1, end - 1);
    const inner = name === 'url' ? raw : normalizeFunctions(raw, decl, diagnostics);
    if (!functions.has(name)) diagnostics.push(cssDiagnostic(decl, 'error', 'unsupported-css-function', `${match[1]}() is outside the validated RmlUi value grammar.`));
    if (name === 'var' && !/^--[-_a-zA-Z][\w-]*(?:\s*,[\s\S]*)?$/.test(inner.trim())) diagnostics.push(cssDiagnostic(decl, 'error', 'invalid-css-variable', 'var() requires a custom property name and an optional fallback.'));
    let replacement = `${match[1]}(${inner})`;
    if (['rgb', 'rgba', 'hsl', 'hsla'].includes(name)) replacement = normalizeColor(name, inner, decl, diagnostics);
    else if (name === 'minmax') replacement = `minmax(${inner.replace(/^\s*0\s*,/, '0px,')})`;
    else if (name === 'translate3d') {
      const args = splitTopLevel(inner, /,/);
      if (args.length === 3 && /^[-+]?0(?:px)?$/.test(args[2])) replacement = `translate(${args.slice(0, 2).join(',')})`;
    } else if (name === 'scale3d') {
      const args = splitTopLevel(inner, /,/);
      if (args.length === 3 && +args[2] === 1) replacement = `scale(${args.slice(0, 2).join(',')})`;
    }
    result += value.slice(cursor, start) + replacement;
    cursor = end; pattern.lastIndex = end;
  }
  return result + value.slice(cursor);
}

function expandTokenCandidates(value, tokens, visited = new Set()) {
  let expanded = value;
  for (const match of value.matchAll(/\bvar\(\s*(--[-_a-zA-Z][\w-]*)/g)) {
    if (visited.has(match[1])) continue;
    const next = new Set(visited); next.add(match[1]);
    expanded += ` ${[...(tokens.get(match[1]) ?? [])].map(candidate => expandTokenCandidates(candidate, tokens, next)).join(' ')}`;
  }
  return expanded;
}

function requiredBy(decl, tokens) {
  const prop = decl.prop.toLowerCase(), value = expandTokenCandidates(decl.value, tokens);
  const required = new Set();
  if (prop.startsWith('--') || /\bvar\(/.test(value)) required.add('css.variables');
  if (prop.startsWith('grid-') || /^(?:inline-)?grid$/.test(value) && prop === 'display') required.add('css.grid');
  if (prop === 'animation' || prop === 'transition') required.add('css.motion');
  if (prop === 'transform' && value !== 'none') required.add(transform3d.test(value) ? 'render.transform3d' : 'render.transform2d');
  if (/^perspective/.test(prop) && value !== 'none' || prop === 'transform-origin-z' && !/^0(?:px)?$/.test(value)) required.add('render.transform3d');
  if ((prop === 'box-shadow' || prop === 'mask-image') && value !== 'none') required.add('render.layers');
  if (prop === 'box-shadow' && value !== 'none') {
    const hasBlur = /\bvar\(/.test(value) || postcss.list.comma(value).some(shadow => {
      const offsets = postcss.list.space(shadow).filter(part => numeric.test(part) || length.test(part));
      return offsets.length > 2 && parseFloat(offsets[2]) > 0;
    });
    if (hasBlur) required.add('render.filters');
  }
  if ((prop === 'filter' || prop === 'backdrop-filter') && value !== 'none') { required.add('render.layers'); required.add('render.filters'); }
  if ((prop === 'decorator' || prop === 'mask-image') && shader.test(value)) required.add('render.shaders');
  if (prop === 'decorator' && /\bue-material(?:-border)?\(/.test(value)) required.add('render.ui-material');
  return [...required];
}

function validateDeclarationValue(decl, diagnostics) {
  const prop = decl.prop.toLowerCase(), value = decl.value.trim();
  if (prop.startsWith('--')) return;
  const fail = message => diagnostics.push(cssDiagnostic(decl, 'error', 'unsupported-css-value', message));
  if (prop === 'transform' && value !== 'none' && !/^var\(/.test(value)) {
    for (const part of splitTopLevel(value, /\s/)) {
      const call = /^([a-z][a-z0-9]*)\(([\s\S]*)\)$/i.exec(part);
      if (!call) { fail(`Invalid transform ${part}; expected a native transform function.`); continue; }
      const name = call[1].toLowerCase(), args = splitTopLevel(call[2], /,/);
      const counts = { translate: [1, 2], translatex: [1], translatey: [1], translatez: [1], translate3d: [3], scale: [1, 2], scalex: [1], scaley: [1], scalez: [1], scale3d: [3], rotate: [1], rotatex: [1], rotatey: [1], rotatez: [1], rotate3d: [4], skew: [1, 2], skewx: [1], skewy: [1], matrix: [6], matrix3d: [16], perspective: [1] };
      if (!counts[name]?.includes(args.length)) { fail(`Unsupported transform ${name} argument count.`); continue; }
      args.forEach((arg, index) => {
        if (/^var\(/.test(arg)) return;
        const isAngle = name.startsWith('rotate') && (name !== 'rotate3d' || index === 3) || name.startsWith('skew');
        const isLength = name.startsWith('translate') || name === 'perspective';
        const valid = isAngle ? arg === '0' || /^[-+]?(?:\d*\.)?\d+(?:deg|rad)$/.test(arg)
          : isLength ? arg === '0' || length.test(arg) : numeric.test(arg);
        if (!valid) fail(`${name} has an invalid ${isAngle ? 'angle' : isLength ? 'length' : 'number'}: ${arg}.`);
      });
    }
  }
  if (prop === 'decorator') {
    for (const match of value.matchAll(/\bue-material(?:-border)?\(([^)]*)\)/g)) {
      if (!/^[-_.a-zA-Z0-9]+$/.test(match[1])) fail('Unreal material decorators require a registered alias, never an asset path or expression.');
    }
  }
  // Variable substitution is performed by ElementStyle. Literal parts and function
  // grammar are checked separately, while renderer requirements include token values.
  if (/\bvar\(/.test(value)) return;
  if (/^(?:width|height|min-width|min-height|max-width|max-height|top|right|bottom|left|flex-basis|font-size|letter-spacing|row-gap|column-gap)$/.test(prop)) {
    const keyword = value === 'auto' && /^(?:width|height|top|right|bottom|left|flex-basis)$/.test(prop)
      || value === 'none' && /^max-/.test(prop) || value === 'normal' && prop === 'letter-spacing';
    if (!keyword && value !== '0' && !length.test(value)) fail(`${prop} expects a supported length${keyword ? '' : ' or a property-specific keyword'}, not ${value}.`);
  }
  if (/^(?:margin|padding)(?:-(?:top|right|bottom|left))?$/.test(prop) || prop === 'gap' || /^(?:border-width|border-radius|border-(?:top|right|bottom|left)-width|border-(?:top|bottom)-(?:left|right)-radius)$/.test(prop)) {
    const parts = postcss.list.space(value);
    if (!parts.length || parts.length > 4 || parts.some(item => item !== '0' && !length.test(item) && !(prop.startsWith('margin') && item === 'auto'))) fail(`${prop} requires supported lengths; slash radii, calculations and intrinsic sizing are not part of this profile.`);
  }
  if (prop === 'background' || /^(?:color|background-color|caret-color|image-color|border-(?:top|right|bottom|left)-color)$/.test(prop)) {
    const hex = /^#(?:[0-9a-f]{3,4}|[0-9a-f]{6}|[0-9a-f]{8})$/i;
    const functional = /^(?:rgb|rgba|hsl|hsla|lab|lch|oklab|oklch)\([\s\S]*\)$/;
    if (!hex.test(value) && !functional.test(value) && !namedColors.has(value) && !(prop === 'caret-color' && value === 'auto')) fail(`${prop} requires a native color value. Use image/gradient decorators for backgrounds and a complete inherited token instead of currentColor.`);
  }
  if (/^(?:flex-grow|flex-shrink|order|font-weight|z-index)$/.test(prop) && !numeric.test(value) && !(prop === 'font-weight' && /^(?:normal|bold)$/.test(value)) && !(prop === 'z-index' && value === 'auto')) fail(`${prop} requires a number or its native keyword.`);
}

export function applyCssProfile(root, options, diagnostics) {
  const profileName = options.profile ?? 'legacy', profile = resolveProfile(profileName);
  if (profileName === 'legacy') return createCapabilities('legacy');
  if (options.mode && !['strict', 'degrade'].includes(options.mode)) throw new Error(`Unknown CSS compatibility mode: ${options.mode}`);
  for (const feature of options.allowDegrade ?? []) if (!Object.hasOwn(capabilityCatalog.features, feature)) throw new Error(`Unknown degradable capability: ${feature}`);
  const required = new Set(['css.core']), degraded = new Set();
  const tokens = new Map();
  root.walkDecls(decl => {
    if (!decl.prop.startsWith('--')) return;
    if (!tokens.has(decl.prop)) tokens.set(decl.prop, new Set());
    tokens.get(decl.prop).add(decl.value);
  });
  root.walkAtRules(rule => {
    if (!['keyframes', 'media', 'font-face', 'spritesheet', 'decorator'].includes(rule.name.toLowerCase())) diagnostics.push(cssDiagnostic(rule, 'error', 'unsupported-css-at-rule', `@${rule.name} is outside this profile.`));
  });
  root.walkRules(rule => {
    if (/::|:(?:has|is|where)\(/i.test(rule.selector) || (rule.selector.match(/:not\(/g) ?? []).length > 1 || /:not\([^)]*[,(]/.test(rule.selector)) diagnostics.push(cssDiagnostic(rule, 'error', 'unsupported-css-selector', `Selector ${rule.selector} requires an unimplemented browser selector contract.`));
  });
  root.walkDecls(decl => {
    const prop = decl.prop.toLowerCase();
    if (!prop.startsWith('--') && !properties.has(prop)) {
      diagnostics.push(cssDiagnostic(decl, 'error', 'unsupported-css-property', `Unsupported RmlUi CSS property: ${decl.prop}`)); return;
    }
    if (prop.startsWith('--') && !/^--[-_a-zA-Z][\w-]*$/.test(decl.prop)) {
      diagnostics.push(cssDiagnostic(decl, 'error', 'invalid-custom-property-name', `Invalid custom property ${decl.prop}.`)); return;
    }
    const original = decl.value;
    decl.value = normalizeFunctions(decl.value, decl, diagnostics);
    if (decl.value !== original) diagnostics.push({ ...cssDiagnostic(decl, 'info', 'css-value-normalized', 'Converted CSS value to the native RCSS representation.'), originalValue: original });
    if (enums[prop] && !/\bvar\(/.test(decl.value) && !enums[prop].includes(decl.value)) diagnostics.push(cssDiagnostic(decl, 'error', 'unsupported-css-value', `${prop}: ${decl.value} has no supported core semantics.`));
    if (/\b(?:dvh|dvw|svh|svw|lvh|lvw|cqw|cqh|cqi|cqb)\b/.test(decl.value) || /\d(?:dvh|dvw|svh|svw|lvh|lvw|cqw|cqh|cqi|cqb)\b/.test(decl.value)) diagnostics.push(cssDiagnostic(decl, 'error', 'unsupported-css-unit', 'Dynamic viewport and container units require an explicit host layout contract.'));
    if (prop === 'opacity' && !/\bvar\(/.test(decl.value) && (!numeric.test(decl.value) || +decl.value < 0 || +decl.value > 1)) diagnostics.push(cssDiagnostic(decl, 'error', 'invalid-opacity', 'opacity must be a scalar between 0 and 1.'));
    validateDeclarationValue(decl, diagnostics);
    if (['transform', 'decorator', 'mask-image'].includes(prop) && /^var\(/.test(decl.value)) {
      const match = /^var\(\s*(--[-_a-zA-Z][\w-]*)/.exec(decl.value);
      if (match && !tokens.has(match[1])) diagnostics.push(cssDiagnostic(decl, 'error', 'untyped-renderer-variable', `The renderer requirements of ${match[1]} are unknown. Declare its values in this stylesheet or keep the function explicit and vary scalar/color arguments.`));
    }
    const features = requiredBy(decl, tokens);
    const unavailable = features.filter(feature => !profile.features.includes(feature));
    if (unavailable.length) {
      const allowed = options.mode === 'degrade' && unavailable.every(feature => options.allowDegrade?.includes(feature));
      diagnostics.push({ ...cssDiagnostic(decl, allowed ? 'warning' : 'error', allowed ? 'renderer-effect-degraded' : 'unsupported-renderer-feature', `${profileName} cannot render ${unavailable.join(', ')}. ${allowed ? 'Removed this declaration under the explicit downgrade policy.' : 'Use a supported effect or explicitly authorize degradation.'}`, allowed ? 'degraded' : 'rejected'), features: unavailable });
      if (allowed) { for (const feature of unavailable) degraded.add(feature); decl.remove(); }
      return;
    }
    for (const feature of features) required.add(feature);
  });
  return createCapabilities(profileName, [...required], [...degraded]);
}
