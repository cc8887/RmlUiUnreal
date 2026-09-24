import { createHash } from 'node:crypto';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import path from 'node:path';
import render from 'dom-serializer';
import { findAll } from 'domutils';
import { compileCss } from './compile-css.mjs';
import { compileDocumentMarkup, compileMarkupTree, throwDiagnostics } from './compile-markup.mjs';
import { mergeCapabilities } from './capabilities.mjs';

function hash(text) {
  return createHash('sha256').update(text).digest('hex');
}

async function writeIfChanged(filePath, contents) {
  let previous;
  try { previous = await readFile(filePath, 'utf8'); } catch { previous = null; }
  if (previous === contents) return false;
  await mkdir(path.dirname(filePath), { recursive: true });
  await writeFile(filePath, contents, 'utf8');
  return true;
}

function localStylesheetHref(node) {
  if (node.name?.toLowerCase() !== 'link') return null;
  const href = node.attribs?.href;
  if (!href || /^(?:[a-z]+:|\/\/|#)/i.test(href)) return null;
  const rel = node.attribs?.rel?.toLowerCase();
  const type = node.attribs?.type?.toLowerCase();
  return rel === 'stylesheet' || type === 'text/css' || type === 'text/rcss' || /\.r?css$/i.test(href) ? href : null;
}

export { compileDocumentMarkup } from './compile-markup.mjs';

export async function compileDocumentFile(inputPath, outputPath, options = {}) {
  const input = path.resolve(inputPath);
  const output = path.resolve(outputPath);
  const source = await readFile(input, 'utf8');
  const inline = compileMarkupTree(source, input, { ...options, allowLinkedStyles: true });
  const { document, diagnostics } = inline;
  const motionRules = [];
  const motionPlayStates = [];
  const capabilityRecords = [inline.capabilities];
  const emittedFiles = [];
  const pendingFiles = [];

  const styleSources = findAll((node) => node.type === 'tag' &&
    (node.name?.toLowerCase() === 'style' || localStylesheetHref(node)), document.children);
  for (const link of styleSources) {
    if (link.name?.toLowerCase() === 'style') {
      const motion = inline.motionByStyle.get(link);
      motionRules.push(...(motion?.rules ?? []));
      motionPlayStates.push(...(motion?.playStates ?? []));
      continue;
    }
    const href = localStylesheetHref(link);
    const sourceCssPath = path.resolve(path.dirname(input), href);
    const relativeCssPath = path.relative(path.dirname(input), sourceCssPath);
    if (relativeCssPath.startsWith('..') || path.isAbsolute(relativeCssPath)) {
      diagnostics.push({ severity: 'error', code: 'stylesheet-outside-document-root', message: `Stylesheet ${href} escapes the document directory.`, source: input, line: 0, column: 0 });
      continue;
    }
    const cssSource = await readFile(sourceCssPath, 'utf8');
    const result = compileCss(cssSource, { ...options, from: sourceCssPath });
    diagnostics.push(...result.diagnostics);
    capabilityRecords.push(result.capabilities);
    motionRules.push(...result.motionManifest.rules);
    motionPlayStates.push(...(result.motionManifest.playStates ?? []));
    const outputCssPath = path.resolve(path.dirname(output), relativeCssPath);
    pendingFiles.push([outputCssPath, result.css]);
    emittedFiles.push({ path: outputCssPath, sha256: hash(result.css) });
  }

  throwDiagnostics(diagnostics);
  for (const [filename, css] of pendingFiles) await writeIfChanged(filename, css);

  const outputMarkup = render(document, { xmlMode: true, encodeEntities: false });
  await writeIfChanged(output, outputMarkup);
  emittedFiles.unshift({ path: output, sha256: hash(outputMarkup) });
  const manifestPath = `${output}.webcompat.json`;
  const capabilities = mergeCapabilities(options.profile ?? 'legacy', capabilityRecords, options.requiredFeatures);
  const motionManifest = motionPlayStates.length
    ? { schemaVersion: 1, rules: motionRules, playStates: motionPlayStates }
    : { schemaVersion: 1, rules: motionRules };
  const manifest = `${JSON.stringify({ schemaVersion: 1, input, inputSha256: hash(source), emittedFiles, diagnostics, capabilities, motionManifest }, null, 2)}\n`;
  await writeIfChanged(manifestPath, manifest);
  return { output, manifestPath, diagnostics, emittedFiles, capabilities, motionManifest };
}
