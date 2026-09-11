import render from 'dom-serializer';
import { findAll } from 'domutils';
import { parseDocument } from 'htmlparser2';
import { compileCss } from './compile-css.mjs';

export function compileMarkupTree(source, from) {
  const document = parseDocument(source, { xmlMode: true, lowerCaseAttributeNames: false, lowerCaseTags: false });
  const diagnostics = [];
  const styleNodes = findAll((node) => node.type === 'tag' && node.name?.toLowerCase() === 'style', document.children);
  for (const style of styleNodes) {
    const cssText = style.children?.map((node) => node.data ?? '').join('') ?? '';
    const result = compileCss(cssText, { from: `${from}#inline-style` });
    diagnostics.push(...result.diagnostics);
    style.children = [{ type: 'text', data: result.css, parent: style, prev: null, next: null }];
  }
  return { document, diagnostics };
}

export function throwDiagnostics(diagnostics) {
  const errors = diagnostics.filter((item) => item.severity === 'error');
  if (errors.length === 0) return;
  const detail = errors.map((item) => `${item.source}:${item.line}:${item.column} ${item.code}: ${item.message}`).join('\n');
  throw new Error(`WebCompat compilation failed:\n${detail}`);
}

export function compileDocumentMarkup(source, options = {}) {
  const from = options.from ?? 'memory.html';
  const { document, diagnostics } = compileMarkupTree(source, from);
  throwDiagnostics(diagnostics);
  return { markup: render(document, { xmlMode: true, encodeEntities: false }), diagnostics };
}
