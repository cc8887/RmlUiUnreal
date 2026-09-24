#!/usr/bin/env node
import { readFile, writeFile } from 'node:fs/promises';
import { compileDocumentMarkup } from './compile-document.mjs';

const [, , inputPath, outputPath] = process.argv;
if (!inputPath || !outputPath) {
  console.error('Usage: node compile-string-cli.mjs <request.json> <response.json>');
  process.exit(2);
}

let response;
try {
  const request = JSON.parse(await readFile(inputPath, 'utf8'));
  if (typeof request.markup !== 'string') throw new Error('Request field markup must be a string.');
  const result = compileDocumentMarkup(request.markup, { from: request.sourcePath || 'memory.html', profile: request.profile, mode: request.mode, allowDegrade: request.allowDegrade });
  response = { success: true, markup: result.markup, diagnostics: result.diagnostics, capabilities: result.capabilities,
    motionManifest: result.motionManifest };
} catch (error) {
  response = { success: false, error: error.message };
}
await writeFile(outputPath, `${JSON.stringify(response)}\n`, 'utf8');
if (!response.success) process.exitCode = 1;
