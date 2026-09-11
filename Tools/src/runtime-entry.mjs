import { compileDocumentMarkup } from './compile-markup.mjs';

const bridge = require('puerts').argv.getByName('bridge');

bridge.OnCompileRequest.Add((markup, sourcePath) => {
  try {
    const result = compileDocumentMarkup(markup, { from: sourcePath || 'memory.html' });
    bridge.Complete(true, result.markup, JSON.stringify(result.diagnostics));
  } catch (error) {
    bridge.Complete(false, '', error instanceof Error ? error.message : String(error));
  }
});
bridge.ReportReady();
