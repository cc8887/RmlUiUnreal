import http from 'node:http';
import { setTimeout as pause } from 'node:timers/promises';
export const sampleCode = 'const int columns = 3;\nfor (int i = 0; i < columns; ++i) {\n    RenderColumn(i);\n}\n';
export const sampleReply = '# A small Grid example\n\nUse **explicit columns** to keep the layout predictable. Each item can still grow with its content.\n\n```cpp\n' + sampleCode + '```\n\n' +
  '| Feature | Layout | Result |\n| :--- | :--- | ---: |\n| Columns | `1fr 1fr 1fr` | 3 |\n| Gap | `16px` | 16 |\n\n' +
  '> Keep the message column readable, then let the outer layout resize.\n\n' +
  '- **中文验证**：流式文本、列表和代码可以一起显示。\n- *Streaming* preserves completed blocks.\n- ~~Old layout~~ becomes a responsive grid.\n\n' +
  'Read the [RmlUi documentation](https://mikke89.github.io/RmlUiDoc/).\n\n' +
  'Raw HTML stays text: <script>alert("test")</script>.\n';
const option = name => { const index = process.argv.indexOf(name); return index >= 0 ? process.argv[index + 1] : undefined; };
const stats = { requests: 0, cancelled: 0, completed: 0, openai: 0, nanochat: 0 };
const server = http.createServer(async (request, response) => {
  if (request.url === '/health' || request.url === '/stats') {
    response.writeHead(200, { 'Content-Type': 'application/json' }); response.end(JSON.stringify({ fixture: true, ...stats })); return;
  }
  if (request.method !== 'POST' || !['/v1/chat/completions', '/chat/completions'].includes(request.url)) { response.writeHead(404).end(); return; }
  let body = '';
  for await (const chunk of request) { body += chunk; if (body.length > 600000) { response.writeHead(413).end(); return; } }
  let data;
  try { data = JSON.parse(body); } catch { response.writeHead(400).end(); return; }
  const input = data.messages?.at(-1)?.content || '';
  const nanochat = request.url === '/chat/completions';
  ++stats.requests; ++stats[nanochat ? 'nanochat' : 'openai'];
  if (input.includes('[error]')) { response.writeHead(503, { 'Content-Type': 'application/json' }).end(JSON.stringify({ error: 'Injected fixture failure' })); return; }
  response.writeHead(200, { 'Content-Type': 'text/event-stream; charset=utf-8', 'Cache-Control': 'no-cache', Connection: 'keep-alive' });
  response.flushHeaders(); request.socket.setNoDelay(true);
  let finished = false, closed = false;
  response.on('close', () => { closed = true; if (!finished) ++stats.cancelled; });
  const write = async text => {
    const bytes = Buffer.from(text);
    // Fragment inside UTF-8, JSON strings, field names and CRLF boundaries.
    for (let offset = 0; offset < bytes.length && !closed; offset += 13) { response.write(bytes.subarray(offset, offset + 13)); await pause(1); }
  };
  if (input.includes('[malformed]')) { await write('data: {invalid json}\n\n'); response.end(); return; }
  const reply = input.includes('[long]') ? sampleReply.repeat(25) : sampleReply;
  const chars = Array.from(reply);
  for (let index = 0; index < chars.length && !closed; index += 28) {
    const content = chars.slice(index, index + 28).join('');
    const event = nanochat ? { token: content } : { choices: [{ index: 0, delta: { content } }] };
    await write(`: heartbeat\r\ndata: ${JSON.stringify(event)}\r\n\r\n`);
    if (input.includes('[broken]') && index > 60) { response.end(); return; }
    await pause(input.includes('[long]') ? 90 : 25);
  }
  if (!closed) { await write(nanochat ? 'data: {"done":true}\n\n' : 'data: [DONE]\n\n'); finished = true; ++stats.completed; response.end(); }
});
const port = Number(option('--port') || 4180);
if (!process.argv.includes('--no-server')) server.listen(port, '127.0.0.1', () => console.log(`Chat fixture: http://127.0.0.1:${port}`));
