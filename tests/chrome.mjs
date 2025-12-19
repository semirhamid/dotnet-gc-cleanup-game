// Minimal headless Chrome driver over the DevTools protocol (Node 22+, no packages).
import { spawn } from 'node:child_process';
import { mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const binary = process.env.CHROME ?? '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
export const sleep = ms => new Promise(r => setTimeout(r, ms));

export async function open(url, width = 1200, height = 800) {
  const profile = mkdtempSync(join(tmpdir(), 'manualgc-chrome-'));
  const chrome = spawn(binary, ['--headless=new', '--disable-gpu', '--no-first-run', '--remote-debugging-port=0',
    `--user-data-dir=${profile}`, `--window-size=${width},${height}`, 'about:blank'], { stdio: ['ignore', 'ignore', 'pipe'] });
  const endpoint = await new Promise((resolve, reject) => {
    let text = '';
    chrome.stderr.on('data', d => { text += d; const m = text.match(/listening on ws:\/\/([^/]+)\//); if (m) resolve(m[1]); });
    setTimeout(() => reject(new Error('Chrome did not start')), 15000);
  });
  const targets = await (await fetch(`http://${endpoint}/json`)).json();
  const socket = new WebSocket(targets.find(t => t.type === 'page').webSocketDebuggerUrl);
  await new Promise(r => { socket.onopen = r; });
  let id = 0;
  const waiting = new Map(), errors = [];
  socket.onmessage = e => {
    const m = JSON.parse(e.data);
    if (m.id) { waiting.get(m.id)(m); waiting.delete(m.id); }
    if (m.method === 'Runtime.exceptionThrown') errors.push(m.params.exceptionDetails.exception?.description ?? m.params.exceptionDetails.text);
  };
  const call = (method, params = {}) => new Promise((resolve, reject) => {
    waiting.set(++id, m => m.error ? reject(new Error(m.error.message)) : resolve(m.result));
    socket.send(JSON.stringify({ id, method, params }));
  });
  await call('Runtime.enable');
  await call('Emulation.setDeviceMetricsOverride', { width, height, deviceScaleFactor: 1, mobile: false });
  await call('Page.navigate', { url });
  const page = {
    errors, call,
    async eval(expression) {
      const r = await call('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true });
      if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description ?? 'evaluation failed');
      return r.result.value;
    },
    async until(expression, ms = 10000) {
      for (const end = Date.now() + ms; Date.now() < end; await sleep(50))
        if (await page.eval(expression).catch(() => false)) return true;
      throw new Error(`timed out waiting for ${expression}`);
    },
    async screenshot(path) { writeFileSync(path, Buffer.from((await call('Page.captureScreenshot')).data, 'base64')); },
    mouse: (type, x, y, buttons = 0) => call('Input.dispatchMouseEvent', { type, x, y, button: buttons || type === 'mouseReleased' ? 'left' : 'none', buttons, clickCount: type === 'mouseMoved' ? 0 : 1 }),
    async close() { socket.close(); chrome.kill(); await sleep(300); rmSync(profile, { recursive: true, force: true, maxRetries: 5, retryDelay: 200 }); },
  };
  return page;
}
