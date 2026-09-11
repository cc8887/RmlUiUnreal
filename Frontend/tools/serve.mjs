import http from 'node:http';
import { readFile } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
const option = name => { const index = process.argv.indexOf(name); return index < 0 ? undefined : process.argv[index + 1]; };
const root = path.resolve(option('--root') || path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../Content/Vue'));
const server = http.createServer(async (request, response) => {
  try {
    const pathname = decodeURIComponent(new URL(request.url, 'http://localhost').pathname);
    const file = path.resolve(root, '.' + pathname);
    if (!file.startsWith(root + path.sep)) { response.writeHead(403).end(); return; }
    const bytes = await readFile(file);
    response.writeHead(200, { 'Content-Length': bytes.length, 'Content-Type': file.endsWith('.json') ? 'application/json' : 'application/octet-stream' });
    response.end(bytes);
  } catch { response.writeHead(404).end(); }
});
const port = Number(option('--port') || process.env.RMLUI_PORT || 4178);
server.listen(port, '127.0.0.1', () => console.log(`Vue update server: http://127.0.0.1:${port}`));
