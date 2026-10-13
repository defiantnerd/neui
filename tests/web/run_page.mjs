// Run a neui web test page headless and pass / fail on its result line.
//
//   node tests/web/run_page.mjs <build-dir> <page.html> [ok-marker] [fail-marker]
//
// Serves <build-dir> over HTTP (wasm needs a real origin), opens the page in
// Chromium through Playwright, echoes the page's console, and exits 0 once a
// console line contains ok-marker (default "SMOKE OK"), 1 on fail-marker
// (default "SMOKE FAILED"), a page error or a 60 s timeout.
// NEUI_WEB_ENGINE=chromium|firefox|webkit picks the engine (default
// chromium); NEUI_WEB_BROWSER=msedge|chrome runs an installed Chromium-based
// browser instead of Playwright's own build.
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { chromium, firefox, webkit } from 'playwright';

const [root, page, ok = 'SMOKE OK', fail = 'SMOKE FAILED'] = process.argv.slice(2);
if (!root || !page) {
  console.error('usage: run_page.mjs <build-dir> <page.html> [ok-marker] [fail-marker]');
  process.exit(2);
}

const mime = { '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript',
               '.wasm': 'application/wasm', '.data': 'application/octet-stream' };
const server = http.createServer((req, res) => {
  const file = path.join(root, decodeURIComponent(new URL(req.url, 'http://x').pathname));
  fs.readFile(file, (err, data) => {
    if (err) { res.writeHead(404); res.end(); return; }
    res.writeHead(200, { 'Content-Type': mime[path.extname(file)] || 'application/octet-stream' });
    res.end(data);
  });
}).listen(0, '127.0.0.1');
await new Promise((r) => server.once('listening', r));

const engines = { chromium, firefox, webkit };
const engine = engines[process.env.NEUI_WEB_ENGINE || 'chromium'];
if (!engine) { console.error('NEUI_WEB_ENGINE must be chromium, firefox or webkit'); process.exit(2); }
const channel = process.env.NEUI_WEB_BROWSER;
const browser = await engine.launch(channel ? { channel } : {});
let verdict = null;
const done = new Promise((resolve) => {
  const finish = (code, why) => { if (verdict === null) { verdict = code; console.log(why); resolve(); } };
  browser.newPage().then(async (p) => {
    p.on('console', (m) => {
      const t = m.text();
      console.log(t);
      if (t.includes(ok)) finish(0, '[run_page] passed');
      else if (t.includes(fail)) finish(1, '[run_page] failed');
    });
    p.on('pageerror', (e) => finish(1, '[run_page] page error: ' + e.message));
    await p.goto(`http://127.0.0.1:${server.address().port}/${page}`);
  });
  setTimeout(() => finish(1, '[run_page] timed out'), 60000);
});
await done;
await browser.close();
server.close();
process.exit(verdict);
