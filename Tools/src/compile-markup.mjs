import render from 'dom-serializer';
import { findAll } from 'domutils';
import { parseDocument } from 'htmlparser2';
import { compileCss } from './compile-css.mjs';
import { mergeCapabilities } from './capabilities.mjs';

export function compileMarkupTree(source, from, options = {}) {
  const document = parseDocument(source, { xmlMode: true, lowerCaseAttributeNames: false, lowerCaseTags: false, withStartIndices: true });
  const diagnostics = [];
  const capabilities = [];
  const motionRules = [];
  const motionPlayStates = [];
  const motionByStyle = new Map();
  const styleNodes = findAll((node) => node.type === 'tag' && node.name?.toLowerCase() === 'style', document.children);
  for (const style of styleNodes) {
    const cssText = style.children?.map((node) => node.data ?? '').join('') ?? '';
    const prefix = source.slice(0, style.children?.[0]?.startIndex ?? style.startIndex ?? 0);
    const result = compileCss(cssText, { ...options, from, lineOffset: prefix.split('\n').length - 1 });
    diagnostics.push(...result.diagnostics);
    capabilities.push(result.capabilities);
    motionRules.push(...result.motionManifest.rules);
    motionPlayStates.push(...(result.motionManifest.playStates ?? []));
    motionByStyle.set(style, result.motionManifest);
    style.children = [{ type: 'text', data: result.css, parent: style, prev: null, next: null }];
  }
  if (options.profile && options.profile !== 'legacy') {
    const links = findAll(node => node.type === 'tag' && node.name?.toLowerCase() === 'link' && node.attribs?.href && (node.attribs.rel?.toLowerCase() === 'stylesheet' || /^text\/(?:r?css)$/i.test(node.attribs.type ?? '') || /\.r?css$/i.test(node.attribs.href)), document.children);
    for (const link of links) {
      if (options.allowLinkedStyles && !/^(?:[a-z]+:|\/\/|#)/i.test(link.attribs.href)) continue;
      diagnostics.push({ severity: 'error', classification: 'rejected', code: 'unvalidated-linked-stylesheet', message: `Strict documents must inline CSS or use a precompiled local stylesheet; ${link.attribs.href} has not passed the renderer profile.`, source: from, line: source.slice(0, link.startIndex ?? 0).split('\n').length, column: 1, selector: '', property: '', value: link.attribs.href });
    }
    const styledNodes = findAll(node => node.type === 'tag' && typeof node.attribs?.style === 'string', document.children);
    for (const node of styledNodes) {
      const prefix = source.slice(0, node.startIndex ?? 0);
      const result = compileCss(`.__inline { ${node.attribs.style} }`, { ...options, from, lineOffset: prefix.split('\n').length - 1 });
      diagnostics.push(...result.diagnostics);
      capabilities.push(result.capabilities);
      if (result.motionManifest.playStates?.length || result.motionManifest.rules.length) {
        diagnostics.push({ severity: 'error', classification: 'rejected', code: 'unsupported-inline-motion-manifest',
          message: 'Native animation controls in a style attribute have no stable element selector; move them to a CSS rule.',
          source: from, line: prefix.split('\n').length, column: 1, selector: '', property: 'style', value: node.attribs.style });
      }
      node.attribs.style = result.css.slice(result.css.indexOf('{') + 1, result.css.lastIndexOf('}')).trim();
    }
  }
  return { document, diagnostics, motionByStyle, capabilities: mergeCapabilities(options.profile ?? 'legacy', capabilities, options.requiredFeatures),
    motionManifest: motionPlayStates.length
      ? { schemaVersion: 1, rules: motionRules, playStates: motionPlayStates }
      : { schemaVersion: 1, rules: motionRules } };
}

export function throwDiagnostics(diagnostics) {
  const errors = diagnostics.filter((item) => item.severity === 'error');
  if (errors.length === 0) return;
  const detail = errors.map((item) => `${item.source}:${item.line}:${item.column} ${item.code}: ${item.message}`).join('\n');
  throw new Error(`WebCompat compilation failed:\n${detail}`);
}

export function compileDocumentMarkup(source, options = {}) {
  const from = options.from ?? 'memory.html';
  const { document, diagnostics, capabilities, motionManifest } = compileMarkupTree(source, from, options);
  throwDiagnostics(diagnostics);
  return { markup: render(document, { xmlMode: true, encodeEntities: false }), diagnostics, capabilities, motionManifest };
}
