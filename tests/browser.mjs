// The web surface in a real browser: headless Chromium, driven over the
// DevTools protocol with what a user does (keys, a click on a row, typing
// Korean text and Enter), each step waiting for the page to show its effect;
// and what a screen reader gets from it, read from Chromium's accessibility
// tree: roles, names, the list's focus and selection, each option's place in
// the whole list, and a focused list that survives redraws.
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
  await send('Accessibility.enable');
  const role = n => n.role?.value, name = n => n.name?.value;
  const prop = (n, key) => n.properties?.find(p => p.name === key)?.value;
  const tree = async () => {
    const nodes = (await send('Accessibility.getFullAXTree')).nodes.filter(n => !n.ignored);
    const listbox = nodes.find(n => role(n) === 'listbox');
    const options = nodes.filter(n => role(n) === 'option');
    const active = prop(listbox, 'activedescendant')?.relatedNodes?.[0]?.backendDOMNodeId;
    return { nodes, listbox, options, active };
  };
  const check = (what, holds, seen) => {
    if (!holds) throw new Error(`${what}: ${JSON.stringify(seen)}`);
    steps.push(what);
  };
  let a = await tree();
  const heading = a.nodes.find(n => role(n) === 'heading');
  check('the title is a heading of level 1', heading && name(heading) === 'To do' && prop(heading, 'level')?.value === 1,
    heading);
  check('the list is a focused listbox named for its view', name(a.listbox) === 'tasks_list' &&
    prop(a.listbox, 'focused')?.value === true, a.listbox);
  // Set size and position are not in the DevTools tree: read what Chromium maps to the platform's.
  const places = `[...document.querySelectorAll('[role=option]')].map(o => o.getAttribute('aria-posinset') + '/' + o.getAttribute('aria-setsize')).join()`;
  check('its options are the tasks, each knowing its place in the whole list', a.options.map(name).join() === 'a,b,c' &&
    await evaluate(places) === '1/3,2/3,3/3', [a.options.map(name), await evaluate(places)]);
  check('the focused task is selected and the active descendant', a.options.map(o => prop(o, 'selected')?.value).join() ===
    'false,false,true' && a.active === a.options[2].backendDOMNodeId, a.options);
  const keys = a.nodes.filter(n => role(n) === 'button' && name(n));
  check('keys are named buttons', keys.length >= 2 && keys.some(k => /up/i.test(name(k))), keys.map(name));
  const list = a.listbox.backendDOMNodeId;
  await until('title and field', `document.querySelector('h1').textContent === 'To do' && document.querySelector('dd').textContent === '3 of 3'`);
  await key('ArrowUp', 'ArrowUp', 38);
  await until('ArrowUp moves the focus', `${selected} === 1`);
  a = await tree();
  check('a screen reader follows it: the same list, still focused, its active descendant moved',
    a.listbox.backendDOMNodeId === list && prop(a.listbox, 'focused')?.value === true &&
    a.active === a.options[1].backendDOMNodeId && prop(a.options[1], 'selected')?.value === true, a.options);
  const box = await evaluate(`(() => { const r = document.querySelectorAll('li')[0].getBoundingClientRect(); return [r.x + r.width / 2, r.y + r.height / 2]; })()`);
  await send('Input.dispatchMouseEvent', { type: 'mousePressed', x: box[0], y: box[1], button: 'left', clickCount: 1 });
  await send('Input.dispatchMouseEvent', { type: 'mouseReleased', x: box[0], y: box[1], button: 'left', clickCount: 1 });
  await until('a click focuses its row', `${selected} === 0`);
  await key('Space', ' ', 32, ' ');
  await until('Space marks it done, muted', `document.querySelectorAll('li')[0].classList.contains('muted') && document.querySelector('li .badge')?.textContent === 'done'`);
  await until('the count of open tasks follows', `document.querySelector('dd').textContent === '2 of 3'`);
  await evaluate(`document.getElementById('entry').focus()`);
  await send('Input.insertText', { text: '브라우저' });
  await key('Enter', 'Enter', 13, '\r');
  await until('typed text becomes a task', `document.querySelectorAll('li').length === 4 && document.querySelectorAll('li')[3].textContent === '브라우저' && ${selected} === 3`);
  await until('the input is emptied', `document.getElementById('entry').value === ''`);
  a = await tree();
  const done = a.options[0];
  check('the added task is option 4 of 4, and the done one still says so', a.options.length === 4 &&
    await evaluate(places) === '1/4,2/4,3/4,4/4' && name(a.options[3]) === '브라우저' && /done/.test(name(done)),
    [a.options.map(name), await evaluate(places)]);
  const entry = a.nodes.find(n => role(n) === 'textbox');
  check('the text input is named for its input', entry && name(entry) === 'add', entry);
  await finish(true);
} catch (error) {
  await finish(false, String(error.message || error));
}
