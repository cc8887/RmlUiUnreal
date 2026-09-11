#!/usr/bin/env node
import { compileDocumentFile } from './compile-document.mjs';

const [, , input, output] = process.argv;
if (!input || !output) {
  console.error('Usage: npm run compile -- <input.html> <output.html>');
  process.exit(2);
}

try {
  const result = await compileDocumentFile(input, output);
  for (const item of result.diagnostics) {
    console.warn(`${item.source}:${item.line}:${item.column} ${item.code}: ${item.message}`);
  }
  console.log(`WebCompat: emitted ${result.emittedFiles.length} file(s); manifest ${result.manifestPath}`);
} catch (error) {
  console.error(error.message);
  process.exit(1);
}
