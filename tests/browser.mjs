// The web surface in a real browser: headless Chromium, driven over the
// DevTools protocol with what a user does (keys, a click on a row, typing
// Korean text and Enter), each step waiting for the page to show its effect.
//
// node tests/browser.mjs URL   (the app: examples/todo.aup with tasks a, b, c)
// Prints one JSON line: the steps that held.
import { spawn } from 'node:child_process';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const url = process.argv[2];
const profile = mkdtempSync(join(tmpdir(), 'vega-browser-'));
const chromium = spawn('chromium', ['--headless=new', '--remote-debugging-port=0', `--user-data-dir=${profile}`,
  '--no-first-run', '--no-default-browser-check', '--disable-gpu', '--window-size=900,700', 'about:blank'],
  { stdio: ['ignore', 'ignore', 'pipe'] });
const endpoint = await new Promise((resolve, reject) => {
  let seen = '';
  chromium.stderr.on('data', chunk => {
    seen += chunk;
    const found = seen.match(/DevTools listening on (ws:\/\/\S+)/);
    if (found) resolve(found[1]);
  });
  chromium.on('exit', code => reject(new Error(`chromium exited ${code}: ${seen.slice(-400)}`)));
  setTimeout(() => reject(new Error('chromium did not start')), 20000);
});

const socket = new WebSocket(endpoint);
await new Promise(resolve => socket.addEventListener('open', resolve, { once: true }));
let next = 0;
const waiting = new Map();
socket.addEventListener('message', event => {
  const message = JSON.parse(event.data);
  if (message.id && waiting.has(message.id)) {
    const { resolve, reject } = waiting.get(message.id);
    waiting.delete(message.id);
    if (message.error) reject(new Error(JSON.stringify(message.error)));
    else resolve(message.result);
  }
});
let session;
const send = (method, params = {}) => new Promise((resolve, reject) => {
  const id = ++next;
  waiting.set(id, { resolve, reject });
  socket.send(JSON.stringify(session ? { id, method, params, sessionId: session } : { id, method, params }));
});

const steps = [];
async function finish(ok, error) {
  console.log(JSON.stringify({ ok, steps, error }));
  chromium.kill();
  rmSync(profile, { recursive: true, force: true });
  process.exit(ok ? 0 : 1);
}

try {
  const { targetId } = await send('Target.createTarget', { url });
  ({ sessionId: session } = await send('Target.attachToTarget', { targetId, flatten: true }));
  const evaluate = async expression => (await send('Runtime.evaluate', { expression, returnByValue: true })).result.value;
  const until = async (what, expression) => {
    for (const end = Date.now() + 10000; Date.now() < end;) {
      if (await evaluate(expression)) { steps.push(what); return; }
      await new Promise(resolve => setTimeout(resolve, 50));
    }
    throw new Error(`${what}: never true: ${expression}; page: ${await evaluate('document.body.innerText')}`);
  };
  const selected = `[...document.querySelectorAll('li')].findIndex(li => li.getAttribute('aria-selected') === 'true')`;
  const key = async (code, key, keyCode, text) => {
    await send('Input.dispatchKeyEvent', { type: 'keyDown', code, key, windowsVirtualKeyCode: keyCode, text });
    await send('Input.dispatchKeyEvent', { type: 'keyUp', code, key, windowsVirtualKeyCode: keyCode });
  };

  await until('three tasks, the last focused', `document.querySelectorAll('li').length === 3 && ${selected} === 2`);
  await until('title and field', `document.querySelector('h1').textContent === 'To do' && document.querySelector('dd').textContent === '3 tasks'`);
  await key('ArrowUp', 'ArrowUp', 38);
  await until('ArrowUp moves the focus', `${selected} === 1`);
  const box = await evaluate(`(() => { const r = document.querySelectorAll('li')[0].getBoundingClientRect(); return [r.x + r.width / 2, r.y + r.height / 2]; })()`);
  await send('Input.dispatchMouseEvent', { type: 'mousePressed', x: box[0], y: box[1], button: 'left', clickCount: 1 });
  await send('Input.dispatchMouseEvent', { type: 'mouseReleased', x: box[0], y: box[1], button: 'left', clickCount: 1 });
  await until('a click focuses its row', `${selected} === 0`);
  await key('Space', ' ', 32, ' ');
  await until('Space marks it done, muted', `document.querySelectorAll('li')[0].classList.contains('muted') && document.querySelector('li .badge')?.textContent === 'done'`);
  await evaluate(`document.getElementById('entry').focus()`);
  await send('Input.insertText', { text: '브라우저' });
  await key('Enter', 'Enter', 13, '\r');
  await until('typed text becomes a task', `document.querySelectorAll('li').length === 4 && document.querySelectorAll('li')[3].textContent === '브라우저' && ${selected} === 3`);
  await until('the input is emptied', `document.getElementById('entry').value === ''`);
  await finish(true);
} catch (error) {
  await finish(false, String(error.message || error));
}
