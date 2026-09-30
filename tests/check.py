#!/usr/bin/env python3
"""Vega checks: one aup spec, three surfaces.

1. CLI and NUI: examples/todo.aup under random adds, moves, toggles, urgent
   marks and removals prints exactly what a reference Python model of the app
   and of Vega's conventions says (lines for pipes, sentences for speech),
   including failed events on an empty list.
2. TUI: the same runs drawn at several sizes keep the focused row on screen
   with the least scrolling, show the rows around it in order, right-align
   badges by display width (Hangul, CJK and emoji take two cells, combining
   marks none), and evaluate only the cells on screen: a frame costs the
   title, the field, the focus and three cells per visible row, however long
   the list is.
3. Tables: examples/scores.aup aligns its columns by display width and marks
   row tones.
4. Every aup example runs on every surface under random keys without a crash.
5. Stored: surfaces take turns on one store and each continues the list; an
   interactive session's entries are there in the next one.
6. vega check names every convention a spec seems to mean but misses, and
   none in the examples.
9. Hot swap: a new module replaces the running one, states keeping their
   values by name, whether a script swaps it or --watch (web, and the TUI on
   a pseudo-terminal) sees its file written again, even at the same size
   within the same second; a module that does not load leaves the app as it
   was.
7. Painting: the bytes the TUI sends, replayed by a small terminal emulator,
   give the very frame the dump shows, while sending only what changed.
8. Web: the GUI surface's screen JSON matches the model after random keys,
   text and row clicks, holds only the rows asked for around the focus, and
   every open page hears of each change. In headless Chromium (when node and
   chromium are there) the page itself takes keys, a click and typed text.
11. Pixels: the pixel surface draws the TUI's frames with a font's glyphs
   (a Latin monospace font, then Korean from a CJK one): the same text, blank
   cells only background (a focused row's own colour), every other cell
   inked, so no glyph spills into its neighbour.
12. Display: the same frames through KMS on a writeback connector (when the
   device has one) come back from the display controller exactly as drawn,
   and are the pixel surface's frames for the same grid. Live, fed recorded
   evdev events (typing with Shift, Enter, Tab, Space, a touch on a row), it
   ends on the TUI's frame after the same keys and click.
10. Symbols: SVG icons (arcs, compact flags, holes, no viewBox) become
   outlines of the right area, an icon asking for what symbols cannot hold is
   refused by name, damaged files are refused whole, and (when DejaVu Sans is
   there) glyphs keep their exact area and draw within a few levels of
   FreeType's own coverage, and (when there is an OpenGL ES 3.1 device) text
   the GPU composes from an atlas is what the CPU reference draws.
"""
import argparse
import json
import math
import os
import random
import shutil
import subprocess
import sys
import tempfile
import unicodedata
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent


def run(*args, stdin='', env=None):
    return subprocess.run([str(a) for a in args], input=stdin, capture_output=True, text=True, timeout=120,
                          env=env)


def width(text):
    total = 0
    for ch in text:
        if unicodedata.combining(ch) or ch in '‍️':
            continue
        total += 2 if unicodedata.east_asian_width(ch) in 'WF' else 1
    return total


def pictures(text):
    """Control characters as Vega shows them."""
    return ''.join(chr(0x2400 + ord(c)) if ord(c) < 0x20 else '␡' if c == '\x7f' else c for c in text)


class Todo:
    """examples/todo.aup and Vega's reading of it."""

    def __init__(self):
        self.tasks, self.at = [], 0

    def event(self, name, text=None):
        """Returns False when the event fails (a missing item). point takes
        the row as text: a surface focusing it, as a click would."""
        if name == 'point':
            if not 0 <= text < len(self.tasks):
                return False
            self.at = text
            return True
        if name == 'add':
            self.tasks.append({'title': text, 'done': 0, 'urgent': 0})
            self.at = len(self.tasks) - 1
            return True
        last = max(len(self.tasks) - 1, 0)
        if name in ('toggle', 'urgent', 'drop') and not self.tasks:
            return False
        if name == 'up': self.at = max(self.at - 1, 0)
        elif name == 'down': self.at = min(self.at + 1, last)
        elif name == 'toggle': self.tasks[self.at]['done'] = int(not self.tasks[self.at]['done'])
        elif name == 'urgent': self.tasks[self.at]['urgent'] = int(not self.tasks[self.at]['urgent'])
        elif name == 'drop':
            del self.tasks[self.at]
            self.at = min(self.at, max(len(self.tasks) - 1, 0))
        return True

    def left(self):
        """The left field: open tasks of all, a count over the list."""
        return f"{sum(1 for t in self.tasks if not t['done'])} of {len(self.tasks)}"

    @staticmethod
    def badge(task):
        return 'done' if task['done'] else 'urgent' if task['urgent'] else ''

    @staticmethod
    def tone(task):
        return 'muted' if task['done'] else 'danger' if task['urgent'] else 'normal'

    def cli(self):
        lines = ['To do', f'left: {self.left()}', f'tasks_list ({len(self.tasks)})']
        for i, task in enumerate(self.tasks):
            line = ('> ' if i == self.at else '  ') + pictures(task['title'])
            if self.badge(task):
                line += f' [{self.badge(task)}]'
            if self.tone(task) != 'normal':
                line += f' ({self.tone(task)})'
            lines.append(line)
        return lines

    @staticmethod
    def spoken(text):
        text = text.rstrip(''.join(chr(c) for c in range(0x21)))
        return ''.join(' ' if ord(c) < 0x20 else c for c in text)

    def focus(self):
        if not self.tasks:
            return 'tasks_list is empty.'
        task = self.tasks[self.at]
        words = [self.spoken(task['title'])] + ([self.badge(task)] if self.badge(task) else [])
        tone = 'danger, ' if self.tone(task) == 'danger' else ''
        return f'Item {self.at + 1} of {len(self.tasks)}: {tone}' + ', '.join(words) + '.'

    def say(self):
        n = len(self.tasks)
        lines = ['To do.', f'left: {self.left()}.', f'tasks_list, {n} item{"" if n == 1 else "s"}.']
        return lines + ([self.focus()] if n else [])


KEYS = {'up': 'ArrowUp', 'down': 'ArrowDown', 'toggle': 'Space', 'urgent': 'KeyU', 'drop': 'Delete'}
WORDS = ['milk', '우유 사기', 'call 엄마', '日本語', '😀 party', 'café', 'tab\there', 'x' * 60, '', 'a\nb',
         'ok']


def cli_and_say(vega, module, seeds, steps=300):
    compared = failures = 0
    for seed in seeds:
        rng = random.Random(seed)
        model, script, expected_cli, expected_say = Todo(), [], [], []
        for _ in range(steps):
            roll = rng.random()
            if roll < 0.1:
                row = rng.randrange(len(model.tasks) + 2)
                script.append(f'point tasks_list {row}')
                ok, name = model.event('point', row), 'point'
            elif roll < 0.35:
                word = rng.choice(WORDS)
                script.append('text ' + (word.encode().hex() or '-'))
                ok, name = model.event('add', word), 'add'
            else:
                name = rng.choice(list(KEYS) + ['drop'])
                script.append(f'key {KEYS[name]}')
                ok = model.event(name)
            expected_say.append(model.focus() if ok else f'Could not {name}.')
            failures += not ok
            if rng.random() < 0.2:
                script.append('show')
                expected_cli += model.cli()
                expected_say += model.say()
        text = '\n'.join(script) + '\n'
        cli = run(vega, 'cli', module, stdin=text)
        assert cli.returncode == 0 and cli.stdout.splitlines() == expected_cli, \
            (seed, [(a, b) for a, b in zip(cli.stdout.splitlines(), expected_cli) if a != b][:3])
        say = run(vega, 'say', module, stdin=text)
        assert say.returncode == 0 and say.stdout.splitlines() == expected_say, \
            (seed, [(a, b) for a, b in zip(say.stdout.splitlines(), expected_say) if a != b][:3])
        compared += len(expected_cli) + len(expected_say)
    return compared, failures


def stored_surfaces(vega, module, work, seeds, steps=240):
    """One store, several surfaces in turn: each run continues the list where
    the last one left it, and prints or says what the model says."""
    compared = 0
    for seed in seeds:
        rng = random.Random(seed + 500)
        model, store = Todo(), work / f'store-{seed}'
        cuts = sorted(rng.sample(range(1, steps), 6))
        for number, (start, end) in enumerate(zip([0] + cuts, cuts + [steps])):
            surface = ['cli', 'say'][number % 2]
            script, expected = [], []
            for _ in range(start, end):
                roll = rng.random()
                if roll < 0.1:
                    row = rng.randrange(len(model.tasks) + 1)
                    script.append(f'point tasks_list {row}')
                    ok, name = model.event('point', row), 'point'
                elif roll < 0.4:
                    word = rng.choice(WORDS)
                    script.append('text ' + (word.encode().hex() or '-'))
                    ok, name = model.event('add', word), 'add'
                else:
                    name = rng.choice(list(KEYS) + ['drop'])
                    script.append(f'key {KEYS[name]}')
                    ok = model.event(name)
                if surface == 'say':
                    expected.append(model.focus() if ok else f'Could not {name}.')
            script.append('show')
            expected += model.cli() if surface == 'cli' else model.say()
            out = run(vega, surface, module, '--store', store, stdin='\n'.join(script) + '\n')
            assert out.returncode == 0 and out.stdout.splitlines() == expected, \
                (seed, number, [(a, b) for a, b in zip(out.stdout.splitlines(), expected) if a != b][:3])
            compared += len(expected)
        frame = run(vega, 'tui', module, '--store', store, '--dump', '40x12', stdin='show\n').stdout.splitlines()
        assert frame[2].startswith(f'|tasks_list ({len(model.tasks)})'), frame
    return compared


def frames(output, height):
    lines = output.splitlines()
    for start in range(0, len(lines), height + 1):
        frame, cells = lines[start:start + height], lines[start + height]
        assert len(frame) == height and cells.startswith('cells '), lines[start:start + height + 1]
        yield frame, int(cells.split()[1])


def tui(vega, module, seeds, sizes, steps=300):
    """Each frame against the model: which rows show, which one is focused,
    where badges end, and how many cells were evaluated."""
    checked = 0
    for columns, rows in sizes:
        area = rows - 5  # title, field, list header, key bar, input line
        for seed in seeds:
            rng = random.Random(seed * 7 + columns)
            model, script, states, scroll = Todo(), [], [], 0
            for step in range(steps):
                roll = rng.random()
                if roll < 0.12 and step:
                    # A click on a line of the last frame: an item line focuses that item.
                    line = rng.randrange(rows)
                    script.append(f'click {line + 1} 3')
                    if 3 <= line < 3 + area and scroll + line - 3 < len(model.tasks):
                        model.event('point', scroll + line - 3)
                elif roll < 0.4:
                    word = rng.choice(WORDS)
                    script.append('text ' + (word.encode().hex() or '-'))
                    model.event('add', word)
                else:
                    name = rng.choice(list(KEYS))
                    script.append(f'key {KEYS[name]}')
                    model.event(name)
                script.append('show')
                n = len(model.tasks)
                if model.at < scroll: scroll = model.at
                if model.at >= scroll + area: scroll = model.at - area + 1
                if scroll + area > n: scroll = max(n - area, 0)
                states.append(([dict(t) for t in model.tasks], model.at))
            result = run(vega, 'tui', module, '--dump', f'{columns}x{rows}', stdin='\n'.join(script) + '\n')
            assert result.returncode == 0, result.stderr
            first = 0
            for (frame, cells), (tasks, at) in zip(frames(result.stdout, rows), states):
                n = len(tasks)
                assert frame[0] == '|To do' and frame[2].startswith(f'|tasks_list ({n})'), frame[:3]
                if at < first: first = at
                if at >= first + area: first = at - area + 1
                if first + area > n: first = max(n - area, 0)
                shown = frame[3:3 + area]
                visible = tasks[first:first + area]
                for i, line in enumerate(shown):
                    if i >= len(visible):
                        assert line == '|', (seed, line)
                        continue
                    task, focused = visible[i], first + i == at
                    body = (line[1:].split('\t')[0] + '  ')[:max(2, len(line[1:].split('\t')[0]))]  # trailing spaces are trimmed
                    assert line[0] == ('>' if focused else '|') and body.startswith('> ' if focused else '  '), (seed, i, line)
                    label = pictures(task['title'])
                    badge = Todo.badge(task)
                    if badge:  # the badge ends at the last column
                        assert body.endswith(badge) and width(body) == columns, (seed, body, width(body))
                    if width(label) + 2 + (len(badge) + 2 if badge else 0) <= columns:
                        assert body[2:].startswith(label), (seed, body, label)
                    else:
                        assert '…' in body, (seed, body)
                    tone = Todo.tone(task)
                    assert (line.split('\t')[1] if '\t' in line else 'normal') == tone, (seed, line, tone)
                assert cells == 2 + (2 + 3 * len(visible) if n else 0), (seed, cells, len(visible))
                checked += 1
    # A long list costs what its screen shows: 2,000 tasks, 7 visible rows.
    script = ['text ' + f'task {i}'.encode().hex() for i in range(2000)] + ['key ArrowUp'] * 3 + ['show']
    frame, cells = next(frames(run(vega, 'tui', module, '--dump', '40x12', stdin='\n'.join(script) + '\n').stdout, 12))
    assert cells == 2 + 2 + 3 * 7 and frame[9] == '>> task 1996' and frame[3] == '|  task 1990', (cells, frame)
    return checked


TONE_NAMES = ['normal', 'muted', 'accent', 'good', 'warning', 'danger']


def replay(stream, screen):
    """Apply terminal bytes to screen (rows of [text, tone, focus] cells) as a
    terminal would: cursor moves, clearing, colors and reverse video, wide
    characters over two cells, combining marks joining the character before."""
    text, row, col, tone, focus, i = stream.decode(), 0, 0, 0, False, 0
    while i < len(text):
        ch = text[i]
        if ch == '\x1b':
            assert text[i + 1] == '[', repr(text[i:i + 8])
            j = i + 2
            while not '@' <= text[j] <= '~':
                j += 1
            params, final = text[i + 2:j], text[j]
            if final == 'H':
                row, col = (int(n or 1) - 1 for n in (params.split(';') + ['1'])[:2])
            elif final == 'J':
                for line in screen:
                    line[:] = [[' ', 0, False] for _ in line]
            elif final == 'm':
                codes = params.split(';')
                focus = '7' in codes
                tone = next((t for code, t in (('36', 2), ('32', 3), ('33', 4), ('31', 5), ('2', 1)) if code in codes), 0)
            i = j + 1
            continue
        cells = width(ch)
        if not cells:
            back = col - 1
            while back > 0 and screen[row][back][0] == '':
                back -= 1
            screen[row][back][0] += ch
        else:
            screen[row][col] = [ch, tone, focus]
            if cells == 2:
                screen[row][col + 1] = ['', tone, focus]
            col += cells
        i += 1


def shown(screen):
    """A replayed screen in the dump's form."""
    lines = []
    for line in screen:
        focus = any(cell[2] for cell in line)
        text = ''.join(cell[0] for cell in line).rstrip(' ')
        first = next((cell for cell in line if cell[0] not in ('', ' ')), None)
        tone = first[1] if first else 0
        lines.append(('>' if focus else '|') + text + ('\t' + TONE_NAMES[tone] if tone else ''))
    return lines


def painting(vega, module, seeds, sizes, steps=200):
    """Every painted frame equals the dumped one; later frames send only what
    changed. Returns frames compared and the mean bytes of a first and of a
    later frame."""
    compared, first_bytes, later_bytes, later = 0, 0, 0, 0
    for columns, rows in sizes:
        for seed in seeds:
            rng = random.Random(seed * 13 + columns)
            script = []
            for _ in range(steps):
                roll = rng.random()
                if roll < 0.25: script.append('text ' + (rng.choice(WORDS).encode().hex() or '-'))
                elif roll < 0.3: script.append(rng.choice(['type 61', 'enter', 'tab']))
                else: script.append('key ' + rng.choice(list(KEYS.values())))
                script.append('show')
            text = '\n'.join(script) + '\n'
            dumped = [frame for frame, _ in frames(run(vega, 'tui', module, '--dump', f'{columns}x{rows}', stdin=text).stdout,
                                                   rows)]
            painted = subprocess.run([str(vega), 'tui', str(module), '--paint', f'{columns}x{rows}'], input=text.encode(),
                                     capture_output=True, timeout=120)
            assert painted.returncode == 0, painted.stderr
            out, at, screen, n = painted.stdout, 0, [[[' ', 0, False] for _ in range(columns)] for _ in range(rows)], 0
            while at < len(out):
                end = out.index(b'\n', at)
                size = int(out[at:end].split()[1])
                replay(out[end + 1:end + 1 + size], screen)
                assert shown(screen) == dumped[n], (seed, n, shown(screen), dumped[n])
                if n: later_bytes, later = later_bytes + size, later + 1
                else: first_bytes += size
                at, n = end + 2 + size, n + 1
            assert n == len(dumped), (n, len(dumped))
            compared += n
    first_mean = first_bytes // (len(sizes) * len(seeds))
    later_mean = later_bytes // later
    assert later_mean * 3 < first_mean, (first_mean, later_mean)
    return compared, first_mean, later_mean


def web(vega, module, work, seeds, steps=150, visible=4):
    """The web surface against the model: POST keys, text and row clicks, then
    the screen JSON's title, field, rows, focus and the window of items."""
    import http.client
    import signal
    import threading
    compared = 0
    for seed in seeds:
        store = work / f'web-{seed}'
        process = subprocess.Popen([str(vega), 'web', str(module), '--port', '0', '--store', str(store)],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        port = json.loads(process.stdout.readline())['port']
        connection = http.client.HTTPConnection('127.0.0.1', port, timeout=10)

        def call(method, path, body=None):
            connection.request(method, path, body.encode() if isinstance(body, str) else body)
            response = connection.getresponse()
            return response.status, response.read()

        heard = []
        stream = http.client.HTTPConnection('127.0.0.1', port, timeout=10)
        stream.request('GET', '/events')
        events = stream.getresponse()
        assert events.getheader('Content-Type') == 'text/event-stream'
        listener = threading.Thread(target=lambda: [heard.append(line) for line in iter(events.readline, b'')
                                                    if line.startswith(b'data:')], daemon=True)
        listener.start()
        status, page = call('GET', '/')
        assert status == 200 and b'EventSource' in page and b"role:'listbox'" in page, page[:80]
        assert call('GET', '/nowhere')[0] == 404
        rng, model, first, part = random.Random(seed + 900), Todo(), 0, None
        for _ in range(steps):
            roll = rng.random()
            if roll < 0.3:
                word = rng.choice(WORDS)
                status, answer = call('POST', '/text', word)
                model.event('add', word)
            elif roll < 0.45 and part is not None:
                row = rng.randrange(len(model.tasks) + 1)
                status, answer = call('POST', '/point', f'{part} {row}')
                assert json.loads(answer)['ok'] == model.event('point', row), (seed, row)
            else:
                name = rng.choice(list(KEYS))
                status, answer = call('POST', '/key', KEYS[name])
                assert json.loads(answer)['ok'] == model.event(name), (seed, name)
            assert status == 200
            status, body = call('GET', f'/screen?rows={visible}&first={first}')
            screen = json.loads(body)
            tasks, n = model.tasks, len(model.tasks)
            (listed,) = screen['parts']
            part = listed['part']
            if model.at < first: first = model.at
            if model.at >= first + visible: first = model.at - visible + 1
            if first + visible > n: first = max(n - visible, 0)
            assert screen['title'] == 'To do' and screen['fields'] == [{'name': 'left', 'text': model.left()}], screen
            assert (listed['rows'], listed['focus'], listed['first']) == (n, model.at if n else None, first), (seed, listed)
            window = tasks[first:first + visible]
            assert [(item['row'], item['label'], item['badge'], item['tone']) for item in listed['items']] == \
                [(first + i, task['title'], Todo.badge(task), Todo.tone(task)) for i, task in enumerate(window)], seed
            assert screen['prompt'] == 'add' and listed['pointable'] and listed['kind'] == 'list'
            compared += 1
        process.send_signal(signal.SIGTERM)
        assert process.wait(timeout=10) == 0, process.stderr.read()
        listener.join(timeout=5)
        assert len(heard) > steps // 2, len(heard)  # a change each time, more or less merged
        again = run(vega, 'cli', module, '--store', store, stdin='show\n').stdout.splitlines()
        assert again == model.cli(), (again, model.cli())
    return compared


class WebSurface:
    """vega web on a free port; stops with SIGTERM."""

    def __init__(self, vega, module, *args):
        self.process = subprocess.Popen([str(vega), 'web', str(module), '--port', '0', *map(str, args)],
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.port = json.loads(self.process.stdout.readline())['port']

    def call(self, method, path, body=None):
        import http.client
        connection = http.client.HTTPConnection('127.0.0.1', self.port, timeout=10)
        try:
            connection.request(method, path, body.encode() if isinstance(body, str) else body)
            response = connection.getresponse()
            return response.status, response.read()
        finally:
            connection.close()

    def stop(self):
        import signal
        self.process.send_signal(signal.SIGTERM)
        assert self.process.wait(timeout=10) == 0, self.process.stderr.read()


def browser(vega, module, work):
    """The page in headless Chromium: keys, a click on a row, typed text."""
    import shutil
    if not shutil.which('node') or not shutil.which('chromium'):
        return 'skipped'
    store = work / 'browser-store'
    assert run(vega, 'cli', module, '--store', store, stdin='text 61\ntext 62\ntext 63\n').returncode == 0
    surface = WebSurface(vega, module, '--store', store)
    try:
        result = run('node', HERE / 'browser.mjs', f'http://127.0.0.1:{surface.port}/')
    finally:
        surface.stop()
    outcome = json.loads(result.stdout)
    assert result.returncode == 0 and outcome['ok'], outcome
    after = run(vega, 'cli', module, '--store', store, stdin='show\n').stdout.splitlines()
    assert after == ['To do', 'left: 3 of 4', 'tasks_list (4)', '  a [done] (muted)', '  b', '  c', '> 브라우저'], after
    return len(outcome['steps'])


def split(vega, module, work, seeds, steps=200):
    """examples/today.aup: two lists filtered from one list share one focus.
    The CLI shows each list's items in order with the focus in whichever
    list holds it; point takes a row of either list; the web surface gives
    the other list no focus."""
    compared = 0
    for seed in seeds:
        rng, tasks, at, script, expected = random.Random(seed + 700), [], 0, [], []
        for _ in range(steps):
            roll = rng.random()
            opened = [i for i, t in enumerate(tasks) if not t['done']]
            closed = [i for i, t in enumerate(tasks) if t['done']]
            if roll < 0.3:
                word = rng.choice(WORDS)
                script.append('text ' + (word.encode().hex() or '-'))
                tasks.append({'title': word, 'done': 0})
                at = len(tasks) - 1
            elif roll < 0.5:
                name, rows = rng.choice([('open', opened), ('done', closed)])
                row = rng.randrange(len(rows) + 1)
                script.append(f'point {name} {row}')
                if row < len(rows): at = rows[row]
            else:
                key = rng.choice(['ArrowUp', 'ArrowDown', 'Space'])
                script.append(f'key {key}')
                if key == 'ArrowUp': at = max(at - 1, 0)
                elif key == 'ArrowDown': at = min(at + 1, max(len(tasks) - 1, 0))
                elif tasks: tasks[at]['done'] = int(not tasks[at]['done'])
            if rng.random() < 0.3:
                script.append('show')
                opened = [i for i, t in enumerate(tasks) if not t['done']]
                closed = [i for i, t in enumerate(tasks) if t['done']]
                expected += ['Today', f'left: {len(opened)} open', f'open ({len(opened)})']
                expected += [('> ' if i == at else '  ') + pictures(tasks[i]['title']) for i in opened]
                expected += [f'done ({len(closed)})']
                expected += [('> ' if i == at else '  ') + pictures(tasks[i]['title']) + ' (muted)' for i in closed]
        out = run(vega, 'cli', module, stdin='\n'.join(script) + '\n')
        assert out.returncode == 0 and out.stdout.splitlines() == expected, \
            (seed, [(a, b) for a, b in zip(out.stdout.splitlines(), expected) if a != b][:3])
        compared += len(expected)
    surface = WebSurface(vega, module)
    try:
        for word in ('a', 'b', 'c'): surface.call('POST', '/text', word)
        surface.call('POST', '/key', 'Space')  # c is done: the focus moves to the done list
        parts = json.loads(surface.call('GET', '/screen?rows=5')[1])['parts']
    finally:
        surface.stop()
    assert [(p['name'], p['rows'], p['focus']) for p in parts] == [('open', 2, None), ('done', 1, 0)], parts
    return compared


def hot_swap(vega, aup, work):
    """Swap a module under a running app: by script, and by --watch in the
    web surface when the file is written again (atomically, as aup watch
    writes it). States keep their values by name; a broken module is refused."""
    import time
    env = dict(os.environ, AUP_PATH=str(ROOT / 'lib'))
    source = (ROOT / 'examples/todo.aup').read_text()
    changed = source.replace('view title = "To do"', 'view title = "Things"') + \
        'view done_count = count(t in tasks where t.done)\n'
    modules = []
    # again differs from after in one letter: a module of the same size, written within the same second.
    for name, text in (('before', source), ('after', changed), ('again', changed.replace('"Things"', '"Thingz"'))):
        (work / f'{name}.aup').write_text(text)
        assert run(aup, 'build', work / f'{name}.aup', '-o', work / f'{name}.pmod', env=env).returncode == 0
        modules.append(work / f'{name}.pmod')
    script = f'text 61\ntext 62\nkey Space\nswap {modules[1]}\nkey ArrowUp\nshow\n'
    out = run(vega, 'cli', modules[0], stdin=script).stdout.splitlines()
    assert out == ['Things', 'left: 1 of 2', 'tasks_list (2)', '> a', '  b [done] (muted)', 'done_count: 1'], out
    # --watch: the web surface takes the new module and tells every page.
    live = work / 'live.pmod'
    shutil.copy(modules[0], live)
    surface = WebSurface(vega, live, '--watch')
    import http.client
    import threading
    heard = []
    stream = http.client.HTTPConnection('127.0.0.1', surface.port, timeout=10)
    stream.request('GET', '/events')
    events = stream.getresponse()
    threading.Thread(target=lambda: [heard.append(line) for line in iter(events.readline, b'') if line.startswith(b'data:')],
                     daemon=True).start()
    def replace(module, title):
        """Write module over the watched file as aup does, and wait for the page to show title."""
        shutil.copy(module, work / 'live.tmp')
        os.replace(work / 'live.tmp', live)
        for _ in range(50):
            screen = json.loads(surface.call('GET', '/screen')[1])
            if screen['title'] == title: return screen
            time.sleep(0.05)
        raise AssertionError(f'never shown: {title}: {screen}')

    try:
        for word in ('a', 'b'): surface.call('POST', '/text', word)
        surface.call('POST', '/key', 'Space')
        time.sleep(0.3)
        before = len(heard)
        screen = replace(modules[1], 'Things')
        assert {'name': 'done_count', 'text': '1'} in screen['fields'], screen
        assert [item['label'] for item in screen['parts'][0]['items']] == ['a', 'b'], screen
        time.sleep(0.2)
        assert len(heard) > before, 'the page hears of the new module'
        assert modules[2].stat().st_size == modules[1].stat().st_size
        replace(modules[2], 'Thingz')
        (work / 'live.tmp').write_bytes(b'not a module')
        os.replace(work / 'live.tmp', live)
        time.sleep(0.6)
        assert json.loads(surface.call('GET', '/screen')[1])['title'] == 'Thingz'  # the broken one is refused
    finally:
        surface.stop()
    shutil.copy(modules[0], work / 'tui.pmod')
    interactive(vega, work / 'tui.pmod', replace=modules[1])
    return len(out)


def timing(vega, module):
    """examples/stopwatch.aup: time is an input. A script sets the time events
    see and ticks the timer; the terminal and the web page tick it on the
    clock, and every page hears of each tick."""
    import fcntl, pty, select, struct, termios, time
    script = ('time 1000\nkey Space\ntime 2500\ntick tick\nshow\ntime 63400\ntick tick\nkey KeyL\n'
              'time 70000\nkey Space\ntick tick\nshow\ntime 71000\nkey KeyR\nshow\n')
    out = run(vega, 'cli', module, stdin=script).stdout.splitlines()
    assert out == ['Stopwatch', 'time: 0:01.5', 'status: running', 'laps_list (0)',
                   'Stopwatch', 'time: 1:09.0', 'status: stopped', 'laps_list (1)', '> 1:02.4 (accent)',
                   'Stopwatch', 'time: 0:00.0', 'status: stopped', 'laps_list (1)', '> 1:02.4 (accent)'], out
    # The terminal, on the clock: start it, and a second and more later it shows that.
    pid, fd = pty.fork()
    if pid == 0:
        # the size before the program starts: set after, its first frames could be drawn at the pty's default
        fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack('HHHH', 8, 30, 0, 0))
        os.execv(str(vega), [str(vega), 'tui', str(module)])
    screen = b''

    def read_for(seconds):
        nonlocal screen
        end = time.time() + seconds
        while time.time() < end:
            if select.select([fd], [], [], 0.05)[0]:
                try:
                    screen += os.read(fd, 65536)
                except OSError:
                    break

    read_for(0.3)
    os.write(fd, b' ')
    read_for(1.4)
    os.write(fd, b'\x03')
    read_for(0.3)
    _, status = os.waitpid(pid, 0)
    os.close(fd)
    assert os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0, status
    grid = [[[' ', 0, False] for _ in range(30)] for _ in range(8)]
    replay(screen, grid)
    final = '\n'.join(shown(grid))
    import re
    seconds = float(re.search(r'time\s+0:(\d\d\.\d)', final).group(1))
    assert 1.2 <= seconds <= 2.5 and 'running' in final, final
    # The web page: every tick is news to every open page.
    surface = WebSurface(vega, module)
    try:
        import http.client
        stream = http.client.HTTPConnection('127.0.0.1', surface.port, timeout=10)
        stream.request('GET', '/events')
        events = stream.getresponse()
        events.readline(), events.readline()
        surface.call('POST', '/key', 'Space')
        heard = 0
        began = time.time()
        while time.time() - began < 1.0:
            if events.readline().startswith(b'data:'): heard += 1
        page = json.loads(surface.call('GET', '/screen')[1])
        field = next(f['text'] for f in page['fields'] if f['name'] == 'time')
        assert heard >= 6 and float(field.split(':')[1]) >= 0.8, (heard, field)
        stream.close()
    finally:
        surface.stop()
    return len(out), heard


BEATS_SPEC = """state beats = 0
input beat = every 20 ms
on beat { beats = beats + 1 }
view count = beats
"""


def clock_set_back(vega, aup, work, clock_back):
    """The terminal's timers wait on the monotonic clock: with the wall clock
    set back an hour 0.2 s in (Pure's build/clock-back.so, LD_PRELOAD), a
    timer of 20 ms goes on ticking (review 004: it stopped until the wall
    clock came back to the tick it had waited for). The ticks are the
    app's, in its store."""
    import pty, select, time
    source, module, store = work / 'beats.aup', work / 'beats.pmod', work / 'beats-store'
    source.write_text(BEATS_SPEC)
    built = run(aup, 'build', source, '-o', module)
    assert built.returncode == 0, built.stderr
    pid, fd = pty.fork()
    if pid == 0:
        os.environ['LD_PRELOAD'] = str(clock_back)
        os.execv(str(vega), [str(vega), 'tui', str(module), '--store', str(store)])
    end = time.time() + 0.8
    while time.time() < end:
        if select.select([fd], [], [], 0.05)[0]:
            try:
                os.read(fd, 65536)
            except OSError:
                break
    os.write(fd, b'\x03')
    time.sleep(0.2)
    _, status = os.waitpid(pid, 0)
    os.close(fd)
    assert os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0, status
    shown_lines = run(vega, 'cli', module, '--store', store, stdin='show\n').stdout.splitlines()
    beats = next(int(line.split(':')[1]) for line in shown_lines if line.startswith('count'))
    assert beats >= 20, shown_lines
    return beats


def hosts(vega, serve, module, work):
    """examples/board.aup, one app and one store for people and programs: the
    CLI adds and moves cards, pure-serve serves the same store as a JSON API
    (a program moves one and adds another), and the CLI then shows what the
    program did, its focus still on the card it was on."""
    import http.client
    store = work / 'board-store'
    script = 'time 1000\ntext ' + 'write docs'.encode().hex() + '\ntext ' + 'ship it'.encode().hex() + '\nkey ArrowRight\nshow\n'
    before = run(vega, 'cli', module, '--store', store, stdin=script).stdout.splitlines()
    assert before[-3:] == ['cards_list (2)', '> ship it [doing] (accent)', '  write docs [to do]'], before
    server = subprocess.Popen([str(serve), '--module', str(module), '--store', str(store), '--port', '0'],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        port = json.loads(server.stdout.readline())['port']

        def call(method, path, body=None):
            connection = http.client.HTTPConnection('127.0.0.1', port, timeout=10)
            headers = {'Content-Type': 'application/json'} if body else {}
            connection.request(method, path, body, headers)
            response = connection.getresponse()
            answer = response.status, response.read()
            connection.close()
            return answer

        assert call('GET', '/views/counts') == (200, b'1 to do, 1 doing, 0 done')
        assert call('POST', '/inputs/post', '{"title": "write docs", "lane": 2}') == (200, b'write docs')
        assert call('POST', '/inputs/post', '{"title": "from api", "lane": 1}') == (200, b'from api')
        rows = json.loads(call('GET', '/views/cards_list')[1])['items']
        assert [(r['label'], r['badge']) for r in rows] == [('from api', 'doing'), ('ship it', 'doing'),
                                                            ('write docs', 'done')], rows
    finally:
        import signal
        server.send_signal(signal.SIGTERM)
        assert server.wait(timeout=10) == 0, server.stderr.read()
    after = run(vega, 'cli', module, '--store', store, stdin='show\n').stdout.splitlines()
    assert after == ['Board', 'counts: 0 to do, 2 doing, 1 done', 'cards_list (3)', '  from api [doing] (accent)',
                     '> ship it [doing] (accent)', '  write docs [done] (muted)'], after
    return len(rows)


def scores(vega, module):
    joins = ['ann', '범수', 'Zoë']
    script = [f'text {name.encode().hex()}' for name in joins]
    script += ['key ArrowUp', 'key KeyW', 'key KeyW', 'key KeyW', 'key ArrowUp', 'key KeyL', 'show']
    out = run(vega, 'cli', module, stdin='\n'.join(script) + '\n').stdout.splitlines()
    assert out == ['Scores', 'board (3)',
                   '  name  score  rank',
                   '> ann   -1     - (danger)',
                   '  범수  3      gold (good)',
                   '  Zoë   0      -'], out
    tui = run(vega, 'tui', module, '--dump', '30x8', stdin='\n'.join(script) + '\n').stdout.splitlines()
    assert tui[1:5] == ['|board (3)', '|  name  score  rank\tmuted', '>> ann   -1     -\tdanger',
                        '|  범수  3      gold\tgood'], tui
    say = run(vega, 'say', module, stdin='show\n' + '\n'.join(script) + '\n').stdout.splitlines()
    assert 'Item 1 of 3: danger, name ann, score -1, rank -.' in say, say
    # The web surface: a table's columns and cells in order, with tones.
    surface = WebSurface(vega, module)
    try:
        for line in script[:-1]:
            command, argument = line.split()
            if command == 'text': surface.call('POST', '/text', bytes.fromhex(argument))
            else: surface.call('POST', '/key', argument)
        board = json.loads(surface.call('GET', '/screen?rows=10')[1])['parts'][0]
    finally:
        surface.stop()
    assert board['kind'] == 'table' and board['columns'] == ['name', 'score', 'rank'] and board['focus'] == 0, board
    assert [(item['cells'], item['tone']) for item in board['items']] == [
        (['ann', '-1', '-'], 'danger'), (['범수', '3', 'gold'], 'good'), (['Zoë', '0', '-'], 'normal')], board
    return len(out)


def interactive(vega, module, store=None, replace=None, cut=False):
    """The terminal surface on a real pseudo-terminal: typed text commits
    with Enter, arrow keys and letters reach the app after Tab, Ctrl-C leaves
    and restores the terminal. With replace, it runs with --watch and replace
    is then written over module: the terminal shows it, with the same tasks.
    With cut, every key comes a byte at a time, 50 ms apart, and an arrow's
    escape sequence 150 ms after its first two bytes: the terminal's reads
    are not its keys (a UTF-8 character or a sequence cut short waits for
    its rest)."""
    import fcntl, pty, select, struct, termios, time
    pid, fd = pty.fork()
    if pid == 0:
        fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack('HHHH', 10, 40, 0, 0))  # before it starts: no race
        os.execv(str(vega), [str(vega), 'tui', str(module)] + (['--store', str(store)] if store else []) +
                 (['--watch'] if replace else []))

    def read_for(seconds):
        data, end = b'', time.time() + seconds
        while time.time() < end:
            ready, _, _ = select.select([fd], [], [], 0.05)
            if ready:
                try:
                    data += os.read(fd, 65536)
                except OSError:
                    break
        return data

    screen = read_for(0.3)
    # Then a click on line 5 (béton's row): a mouse report, press and release.
    for keys in (b'milk\r', 'b\u00e9ton\r'.encode(), b'\t', b'\x1b[A', b' ', b'u', b'\x1b[<0;3;5M\x1b[<0;3;5m'):
        if not cut:
            os.write(fd, keys)
        elif keys == b'\x1b[A':
            os.write(fd, keys[:2])
            screen += read_for(0.15)
            os.write(fd, keys[2:])
        else:
            for k in range(len(keys)):
                os.write(fd, keys[k:k + 1])
                screen += read_for(0.05)
        screen += read_for(0.2)
    if replace:
        shutil.copy(replace, module.with_suffix('.tmp'))
        os.replace(module.with_suffix('.tmp'), module)
        screen += read_for(0.6)
    os.write(fd, b'\x03')
    screen += read_for(0.3)
    _, status = os.waitpid(pid, 0)
    os.close(fd)
    text = screen.decode(errors='replace')
    assert os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0, status
    assert '\x1b[?1049h' in text and text.rstrip().endswith('\x1b[?1049l'), text[-80:]
    # What the terminal ended up showing, from the bytes it was sent.
    grid = [[[' ', 0, False] for _ in range(40)] for _ in range(10)]
    replay(screen, grid)
    final = shown(grid)
    if replace:  # the new title and field, the same tasks, focus and done mark
        assert final[:3] == ['|Things', '|left  1 of 2\tmuted', '|done_count  1\tmuted'], final
        assert final[4].startswith('|  milk') and 'done' in final[4] and final[5].startswith('>> béton'), final
    else:
        assert 'milk' in '\n'.join(final) and '(2)' in final[2] and 'done' in final[3], final
        assert final[4].startswith('>> béton') and final[3].startswith('|  milk'), final  # the click moved the focus
    assert '\x1b[?1000h' in text and '\x1b[?1000l' in text, 'mouse reports on, then off'
    if store:  # what was typed is still there in the next session
        again = run(vega, 'cli', module, '--store', store, stdin='show\n').stdout.splitlines()
        assert again == ['To do', 'left: 1 of 2', 'tasks_list (2)', '  milk [done] (muted)', '> béton'], again
    return 1


BROKEN_SPEC = """use vega
record Item {
  name: text
  size = 0
}
state items: list of Item
state at: index of items
input add = text
input note = text
input up = key ArrowUp
on add { items.append(Item { name: add }) }
on note { items.append(Item { name: note }) }
on up.down { at = at - 1 }
view title = 3
view items_list = each item in items {
  label = item.name
  tone = "red"
  badge = item.size
  size = item.size
}
view sizes = each item in items {
  label = item.size
}
view empty = each item in items {
  tone = vega.muted
}
view missing_focus = 1
view items_list_focus = "0"
"""


def conventions(vega, aup, work, modules):
    """Every convention the broken spec misses, and none in the examples."""
    env = dict(os.environ, AUP_PATH=str(ROOT / 'lib'))
    (work / 'broken.aup').write_text(BROKEN_SPEC)
    built = run(aup, 'build', work / 'broken.aup', '-o', work / 'broken.pmod', env=env)
    assert built.returncode == 0, built.stderr
    result = run(vega, 'check', work / 'broken.pmod')
    found = [line.split(': ', 1)[1] for line in result.stdout.splitlines()]
    assert result.returncode == 1 and found == [
        'title: a title is text; this one shows as a field',
        'items_list.tone: a tone is a number (vega.normal ... vega.danger); the list ignores it',
        "items_list.badge: a list's badge is text; this one is not shown",
        'items_list.size: a list shows label, badge, detail and tone; this column is not shown',
        "sizes.label: a list's label is text; sizes shows as a table",
        'empty: a table with no column to show',
        'missing_focus: names no list or table missing; it shows as a field',
        'items_list_focus: the focused row is a number; items_list shows no focus',
        'note: only the first text input (add) has a place to type into'], result.stdout
    for module in modules.values():
        clean = run(vega, 'check', module)
        assert clean.returncode == 0 and not clean.stdout, clean.stdout
    return len(found)


def robustness(vega, aup, work, rounds):
    """Every aup example, every surface, random keys and text: never a crash."""
    examples = sorted((ROOT.parent / 'aup' / 'examples').glob('*.aup')) + sorted((ROOT / 'examples').glob('*.aup'))
    keys = ['ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight', 'Space', 'Enter', 'Delete', 'Backspace', 'Home',
            'End', 'KeyD', 'KeyS', 'KeyU', 'KeyW', 'KeyL', 'KeyM']
    env = dict(os.environ, AUP_PATH=str(ROOT / 'lib'))
    runs = 0
    for path in examples:
        module = work / (path.stem + '.pmod')
        built = run(aup, 'build', path, '-o', module, env=env)
        assert built.returncode == 0, (path, built.stderr)
        assert run(vega, 'check', module).returncode in (0, 1), path
        rng = random.Random(path.stem)
        for _ in range(rounds):
            script = []
            for _ in range(60):
                roll = rng.random()
                if roll < 0.2: script.append('text ' + rng.choice(WORDS).encode().hex())
                elif roll < 0.3: script.append('show')
                elif roll < 0.35: script.append('frame 3')
                elif roll < 0.4: script.append(rng.choice(['type 61', 'enter', 'tab']))
                elif roll < 0.45: script.append(f'click {rng.randrange(1, 30)} {rng.randrange(1, 90)}')
                else: script.append('key ' + rng.choice(keys))
            text = '\n'.join(script) + '\nshow\n'
            size = rng.choice(['80x24', '12x5', '1x1', '200x3'])
            for surface in (['cli'], ['say'], ['tui', '--dump', size], ['tui', '--paint', size]):
                result = run(vega, surface[0], module, *surface[1:], stdin=text)
                assert result.returncode == 0, (path, surface, result.stderr[-400:])
                runs += 1
        # The web surface under random posts: every answer well formed, and a clean stop.
        surface = WebSurface(vega, module)
        try:
            for _ in range(40):
                roll = rng.random()
                if roll < 0.3: surface.call('POST', '/text', rng.choice(WORDS))
                elif roll < 0.4: surface.call('POST', '/point', f'{rng.randrange(4)} {rng.randrange(4)}')
                else: surface.call('POST', '/key', rng.choice(keys))
                status, body = surface.call('GET', f'/screen?rows={rng.randrange(0, 30)}&first={rng.randrange(9)},1')
                assert status == 200 and isinstance(json.loads(body)['parts'], list), (path, body[:200])
        finally:
            surface.stop()
        runs += 1
    return len(examples), runs


def symbols(tool, work):
    """Build, read back and damage symbol files (docs/design/002-symbol-format.md)."""
    icons = ROOT / 'tests' / 'symbols'
    out = work / 'icons.vsym'
    built = run(tool, 'build', '--svg', f'0xE002={icons / "rounded.svg"}', '--svg', f'0xE000={icons / "circle.svg"}',
                '--svg', f'0xE001={icons / "frame.svg"}', '-o', out)
    assert built.returncode == 0, built.stderr
    info = json.loads(run(tool, 'info', out).stdout)
    units = info['units']
    square = (units / 16) ** 2  # an icon pixel, in square units
    expected = {0xE000: math.pi * 49, 0xE001: 196 - 64, 0xE002: 196 - 16 + 4 * math.pi}
    assert [s['code'] for s in info['symbols']] == sorted(expected), info
    for symbol in info['symbols']:
        area = abs(symbol['area']) / square
        assert abs(area - expected[symbol['code']]) < 2e-3 * expected[symbol['code']], (symbol, expected)
        assert symbol['bounds'] == [units // 16, units // 16, units * 15 // 16, units * 15 // 16], symbol
        assert symbol['advance'] == units, symbol
    refused = run(tool, 'build', '--svg', f'0xE000={icons / "turned.svg"}', '-o', work / 'turned.vsym')
    assert refused.returncode != 0 and 'turned.svg' in refused.stderr and 'transform' in refused.stderr, refused.stderr
    twice = run(tool, 'build', '--svg', f'0xE000={icons / "circle.svg"}', '--svg', f'0xE000={icons / "frame.svg"}',
                '-o', work / 'twice.vsym')
    assert twice.returncode != 0 and 'twice' in twice.stderr, twice.stderr

    picture = work / 'circle.pgm'
    assert run(tool, 'render', out, '0xE000', 32, picture).returncode == 0
    header, pixels = picture.read_bytes().split(b'255\n', 1)
    w, h = map(int, header.split()[1:3])
    assert len(pixels) == w * h and abs(sum(pixels) / 255 - math.pi * 49 * 4) < 2, (w, h, sum(pixels) / 255)
    assert run(tool, 'render', out, '0x41', 32, picture).returncode != 0  # no such symbol

    good = out.read_bytes()
    count = int.from_bytes(good[12:16], 'little')
    records = 28

    def damaged(name, data):
        path = work / f'{name}.vsym'
        path.write_bytes(bytes(data))
        refused = run(tool, 'info', path)
        assert refused.returncode != 0 and refused.stdout == '', (name, refused.stdout)
        return refused.stderr.strip()

    why = [damaged('short', good[:-1]), damaged('magic', b'VEGASYM9' + good[8:])]
    swapped = bytearray(good)
    swapped[records:records + 24], swapped[records + 24:records + 48] = good[records + 24:records + 48], good[records:records + 24]
    why.append(damaged('order', swapped))
    beyond = bytearray(good)
    beyond[records + 8:records + 12] = (1 << 20).to_bytes(4, 'little')
    why.append(damaged('beyond', beyond))
    flagged = bytearray(good)
    flagged[records + 22] = 1
    why.append(damaged('flags', flagged))
    counted = bytearray(good)
    counted[12:16] = (count + 1).to_bytes(4, 'little')
    why.append(damaged('count', counted))
    # A symbol's first point starts a contour on the curve, and its last is
    # on it, so an off point's neighbours never leave the symbol.
    points = records + 24 * count
    first, length = (int.from_bytes(good[records + k:records + k + 4], 'little') for k in (4, 8))
    unmoved = bytearray(good)
    unmoved[points + 4 * first + 2] &= 0xFE
    why.append(damaged('unmoved', unmoved))
    open_end = bytearray(good)
    open_end[points + 4 * (first + length - 1)] |= 1
    why.append(damaged('open', open_end))
    assert len(set(why)) == len(why), why

    report = {'icons': len(info['symbols'])}
    font = Path('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf')
    if font.exists():
        glyphs = work / 'ascii.vsym'
        assert run(tool, 'build', '--font', font, '--codes', '0x20-0x7e', '-o', glyphs).returncode == 0
        lines = run(tool, 'compare', font, glyphs, '--sizes', '12,16,32').stdout.splitlines()
        for line in lines[1:]:
            size = json.loads(line)
            assert size['glyphs'] == 94 and size['mean_difference'] < 3 and size['within_16'] > 0.99, size
        report['glyph_sizes'] = len(lines) - 1
        # Text composed by the GPU from the atlas is what vega_symbol_raster
        # draws at the same pen positions (when there is an OpenGL ES 3.1 device).
        drawn = run(tool, 'gpu', glyphs, '--sizes', '12,32', '--frames', 1)
        if drawn.returncode == 0:
            for line in drawn.stdout.splitlines()[1:]:
                size = json.loads(line)
                assert size['symbols'] == 95 and size['worst'] <= 1 and size['page_glyphs'] > 1000, size
            report['gpu_sizes'] = len(drawn.stdout.splitlines()) - 1
        else:
            assert 'EGL' in drawn.stderr or 'OpenGL' in drawn.stderr, drawn.stderr
        try:
            from fontTools.pens.areaPen import AreaPen
            from fontTools.ttLib import TTFont
        except ImportError:
            return report
        face = TTFont(font)
        outlines, cmap, metrics = face.getGlyphSet(), face.getBestCmap(), face['hmtx']
        shown = json.loads(run(tool, 'info', glyphs).stdout)
        assert shown['units'] == face['head'].unitsPerEm, shown['units']  # a TrueType font's points stay whole
        for symbol in shown['symbols']:
            pen = AreaPen()
            outlines[cmap[symbol['code']]].draw(pen)
            assert abs(abs(pen.value) - abs(symbol['area'])) < 1, (chr(symbol['code']), pen.value, symbol)
            assert symbol['advance'] == metrics[cmap[symbol['code']]][0], (chr(symbol['code']), symbol)
        report['exact_glyphs'] = len(shown['symbols'])
    return report


def pixels(vega, tool, module, work):
    """The pixel surface draws the TUI's grid: the same frames as text, and in
    the image every blank cell is its row's background (the focused row's
    own colour) and every other cell has ink."""
    latin = Path('/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf')
    korean = Path('/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc')
    if not latin.exists() or not korean.exists():
        return None
    # A monospace Latin font first (its arrows are one cell wide), then Hangul
    # from Noto Sans Mono CJK KR (face 6 of the collection) where it lacks them.
    face = work / 'ui.vsym'
    built = run(tool, 'build', '--font', latin, '--codes', '0x20-0x7e,0x2190-0x21ff', '--font', korean, '--face', 6,
                '--codes', '0x3131-0x318e,0xac00-0xd7a3', '-o', face)
    assert built.returncode == 0, built.stderr
    items = ['우유 사기', 'Pure 런타임 문서 읽기', 'aup로 앱 만들기', 'draw pixels']
    script = ''.join(f'text {item.encode().hex()}\n' for item in items) + 'key Space\nshow\nkey ArrowUp\nkey ArrowUp\nshow\n'
    columns, rows = 44, 12
    text = run(vega, 'tui', module, '--dump', f'{columns}x{rows}', stdin=script)
    drawn = run(vega, 'pixels', module, '--symbols', face, '--size', 18, '--grid', f'{columns}x{rows}',
                '--out', work / 'frame', stdin=script)
    assert text.returncode == 0 and drawn.returncode == 0, drawn.stderr
    frames = [line.split() for line in drawn.stdout.splitlines() if line.startswith('frame ')]
    assert [line for line in drawn.stdout.splitlines() if not line.startswith('frame ')] == text.stdout.splitlines()
    assert len(frames) == 2
    lines = text.stdout.splitlines()
    background, focused = (0x1e, 0x1e, 0x2e), (0x31, 0x32, 0x44)
    checked = 0
    for number, (_, path, size) in enumerate(frames):
        data = Path(path).read_bytes()
        header, image = data.split(b'255\n', 1)
        width, height = map(int, header.split()[1:3])
        assert f'{width}x{height}' == size and width % columns == 0 and height % rows == 0 and len(image) == width * height * 3
        cell_width, cell_height = width // columns, height // rows
        frame_lines = lines[number * (rows + 1):number * (rows + 1) + rows]
        for row, line in enumerate(frame_lines):
            shown = line[1:].split('\t')[0]
            color = focused if line[0] == '>' else background
            # each character's first cell, as the grid lays them out
            cells, column = {}, 0
            for character in shown:
                cells[column] = character
                column += width_of(character)
            for column in range(columns):
                x0, y0 = column * cell_width, row * cell_height
                inked = any(tuple(image[3 * ((y0 + j) * width + x0 + i):3 * ((y0 + j) * width + x0 + i) + 3]) != color
                            for j in range(cell_height) for i in range(cell_width))
                character = cells.get(column)
                if character is None and any(start < column < start + width_of(c) for start, c in cells.items()):
                    continue  # the second half of a wide character
                if character in (None, ' '):
                    assert not inked, (number, row, column, line)
                else:
                    assert inked, (number, row, column, character, line)
                checked += 1
    return {'frames': len(frames), 'cells': checked}


def typeset(vega, tool, module, work):
    """The pixel surface setting type, its first font shaping: fonts in order
    of preference (DejaVu Sans for Latin and Arabic, Noto Sans CJK KR for
    Hangul, FreeSans for Devanagari). The frames' text is the TUI's. A label
    in one script, alone in its list, is pixel for pixel what the font that
    has it sets by itself (vega-symbols paragraph): each cluster went to that
    font, shaped and placed as the layout says (ink may pass its row, as
    marks below a line do). With several labels, one too long for its row is
    cut within it, rows without text stay blank, and the text cursor stands
    after the prompt."""
    fonts = {'latin': Path('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf'),
             'hangul': Path('/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc'),
             'deva': Path('/usr/share/fonts/truetype/freefont/FreeSans.ttf')}
    if not all(path.exists() for path in fonts.values()):
        return None
    faces = {name: work / f'type-{name}.vsym' for name in fonts}
    for name, args in (('latin', ['--codes', '0x20-0x7e,0xa0-0xff,0x600-0x6ff,0x2010-0x2027,0x2190-0x21ff',
                                  '--shape', 'Latn,Arab,Zyyy']),
                       ('hangul', ['--face', 1, '--codes', '0x3131-0x318e,0xac00-0xd7a3', '--shape', 'Hang']),
                       ('deva', ['--codes', '0x900-0x97f', '--shape', 'Deva'])):
        built = run(tool, 'build', '--font', fonts[name], *args, '--runs', 300, '-o', faces[name])
        assert built.returncode == 0, built.stderr
    stack = [a for face in faces.values() for a in ('--symbols', face)]
    columns, rows, size = 48, 12, 20
    background, focused, normal = (0x1e, 0x1e, 0x2e), (0x31, 0x32, 0x44), (0xcd, 0xd6, 0xf4)
    metrics = {name: json.loads(run(tool, 'info', face).stdout.split(',"symbols"')[0] + '}') for name, face in faces.items()}
    baseline = lambda name: math.ceil(metrics[name]['ascent'] * size / metrics[name]['units'])

    def frame(items):
        script = ''.join(f'text {item.encode().hex()}\n' for item in items) + 'show\n'
        text = run(vega, 'tui', module, '--dump', f'{columns}x{rows}', stdin=script)
        drawn = run(vega, 'pixels', module, *stack, '--size', size, '--grid', f'{columns}x{rows}', '--out', work / 'type',
                    stdin=script)
        assert text.returncode == 0 and drawn.returncode == 0, drawn.stderr
        lines = text.stdout.splitlines()
        assert [line for line in drawn.stdout.splitlines() if not line.startswith('frame ')] == lines
        (_, path, _), = [line.split() for line in drawn.stdout.splitlines() if line.startswith('frame ')]
        header, image = Path(path).read_bytes().split(b'255\n', 1)
        width, height = map(int, header.split()[1:3])
        return lines, image, width, width // columns, height // rows

    compared = 0
    for label, name in {'Kerning AV Wa To': 'latin', '우유 사기': 'hangul', 'مرحبا بالعالم': 'latin',
                        'नमस्ते दुनिया': 'deva'}.items():
        lines, image, width, cell_width, cell_height = frame([label])
        row = next(r for r, line in enumerate(lines[:rows]) if line[1:].split('\t')[0][2:].rstrip() == label)
        set_alone = run(tool, 'paragraph', faces[name], size, 0, work / 'alone.pgm', label)
        assert set_alone.returncode == 0, set_alone.stderr
        head, gray = (work / 'alone.pgm').read_bytes().split(b'255\n', 1)
        alone_width, alone_height = map(int, head.split()[1:3])
        # The paragraph's pen starts at (size, size + its ascent); the label's at column 2, on the row's baseline.
        dx, dy = 2 * cell_width - size, row * cell_height + baseline('latin') - size - baseline(name)
        # coverage of the label's tone over the background of the row the pixel is in
        under = lambda y: (focused if lines[y // cell_height][0] == '>' else background)[2]
        at = lambda x, y: round((image[3 * (y * width + x) + 2] - under(y)) * 255 / (normal[2] - under(y)))
        ink = 0
        for y in range(alone_height):
            for x in range(alone_width):
                if 255 - gray[y * alone_width + x]:
                    ink += 1
                    assert abs(at(x + dx, y + dy) - (255 - gray[y * alone_width + x])) <= 4, (label, x, y)
        for y in range(row * cell_height, (row + 1) * cell_height):
            for x in range(2 * cell_width, width):
                px, py = x - dx, y - dy
                expected = 255 - gray[py * alone_width + px] if 0 <= px < alone_width and 0 <= py < alone_height else 0
                assert abs(at(x, y) - expected) <= 4, (label, x, y, at(x, y), expected)
                compared += 1
        assert ink > 50, label
    # Several: the long label cut, its ink reaching into its row's last cells.
    long = 'Pure 런타임 ' + 'and a tail far too long to fit ' * 3
    lines, image, width, cell_width, cell_height = frame(['우유 사기', 'Pure 런타임 문서 읽기', long])
    pixel = lambda x, y: tuple(image[3 * (y * width + x):3 * (y * width + x) + 3])
    row = next(r for r, line in enumerate(lines[:rows]) if line[3:].startswith('Pure 런타임 and'))
    color = focused if lines[row][0] == '>' else background
    inked = [x for x in range(width) if any(pixel(x, y) != color for y in range(row * cell_height, (row + 1) * cell_height))]
    assert inked and width - 3 * cell_width <= inked[-1] < width, (inked[-1:], width)
    # Rows without text (and none after a row of text: nothing there passes its row) are blank.
    for row, line in enumerate(lines[:rows]):
        if not line[1:].split('\t')[0].strip() and (not row or not lines[row - 1][1:].split('\t')[0].strip()):
            color = focused if line[0] == '>' else background
            assert all(pixel(x, y) == color for y in range(row * cell_height, (row + 1) * cell_height)
                       for x in range(width)), row
    prompt = rows - 1
    assert any(sum(pixel(x, y) == normal for y in range(prompt * cell_height, (prompt + 1) * cell_height)) >= cell_height // 2
               for x in range(width)), 'no text cursor'
    return {'labels': 4, 'pixels': compared}


def display(vega, module, work):
    """The same frames on a display through KMS, on a writeback connector: each
    comes back from the display controller exactly as drawn, and it is the
    pixel surface's frame for the same grid. None without such a device."""
    card, face = Path('/dev/dri/card1'), work / 'ui.vsym'
    if not card.exists() or not face.exists():
        return None
    items = ['우유 사기', 'Pure 런타임 문서 읽기', 'on the display']
    script = ''.join(f'text {item.encode().hex()}\n' for item in items) + 'key Space\nshow\nkey ArrowUp\nshow\n'
    shown = run(vega, 'display', module, '--symbols', face, '--size', 18, '--card', card, '--writeback', '640x480',
                '--out', work / 'display', stdin=script)
    if shown.returncode == 2 and ('writeback' in shown.stderr or 'cannot open' in shown.stderr):
        return None  # no writeback connector, or no access to the card
    assert shown.returncode == 0, shown.stderr
    frames = [line.split() for line in shown.stdout.splitlines() if line.startswith('frame ')]
    assert len(frames) == 2 and all(frame[3] == '0' for frame in frames), frames
    grid = frames[0][5]
    drawn = run(vega, 'pixels', module, '--symbols', face, '--size', 18, '--grid', grid, '--out', work / 'grid', stdin=script)
    assert drawn.returncode == 0, drawn.stderr
    assert [line for line in drawn.stdout.splitlines() if not line.startswith('frame ')] == \
        [line for line in shown.stdout.splitlines() if not line.startswith('frame ')]
    compared = 0
    for (_, captured, *_), (_, pixel_path, size) in zip(frames, [l.split() for l in drawn.stdout.splitlines() if l.startswith('frame ')]):
        header, image = Path(captured).read_bytes().split(b'255\n', 1)
        full_width = int(header.split()[1])
        head, grid_image = Path(pixel_path).read_bytes().split(b'255\n', 1)
        width, height = map(int, head.split()[1:3])
        for y in range(height):
            assert image[3 * y * full_width:3 * (y * full_width + width)] == grid_image[3 * y * width:3 * (y + 1) * width], y
        compared += width * height
    return {'frames': len(frames), 'pixels': compared}


def live_display(vega, module, work):
    """The display live, fed recorded evdev events (a keyboard typing two items,
    Tab, Space, then a touch on the first item's row): its last frame is the
    TUI's after the same keys and click, and it came back from the display
    controller as drawn."""
    import struct
    card, face = Path('/dev/dri/card1'), work / 'ui.vsym'
    if not card.exists() or not face.exists():
        return None
    display_args = ['--symbols', face, '--size', 18, '--card', card, '--writeback', '640x480']
    first = run(vega, 'display', module, *display_args, '--out', work / 'first', stdin='show\n')
    if first.returncode == 2 and ('writeback' in first.stderr or 'cannot open' in first.stderr):
        return None
    lines = first.stdout.splitlines()
    frame = next(line.split() for line in lines if line.startswith('frame '))
    grid, cell_height = frame[5], int(frame[7].split('x')[1])
    heading = next(i for i, line in enumerate(lines) if line[1:].startswith('tasks_list'))
    codes = {'m': 50, 'i': 23, 'l': 38, 'k': 37, 'b': 48, 'r': 19, 'e': 18, 'a': 30, 'd': 32,
             'enter': 28, 'tab': 15, 'space': 57, 'shift': 42}
    events = []  # (type, code, value): EV_KEY 1, EV_ABS 3, EV_SYN 0
    def key(name, value=None):
        for v in ((1, 0) if value is None else (value,)):
            events.extend([(1, codes[name], v), (0, 0, 0)])
    for letter in 'milk':
        key(letter)
    key('enter')
    key('shift', 1), key('b'), key('shift', 0)
    for letter in 'read':
        key(letter)
    key('enter'), key('tab'), key('space')
    # a touch on the first item's row (the line after the list's heading), in screen pixels
    events.extend([(3, 0, 20), (3, 1, (heading + 1) * cell_height + cell_height // 2), (1, 330, 1), (0, 0, 0),
                   (1, 330, 0), (0, 0, 0)])
    recording = work / 'events.bin'
    recording.write_bytes(b''.join(struct.pack('llHHi', 0, 0, *event) for event in events))
    live = run(vega, 'display', module, *display_args, '--out', work / 'live', '--input', recording)
    assert live.returncode == 0, live.stderr
    last = [line.split() for line in live.stdout.splitlines() if line.startswith('frame ')][-1]
    assert last[3] == '0', last  # came back as drawn
    script = f'type {"milk".encode().hex()}\nenter\ntype {"Bread".encode().hex()}\nenter\ntab\nkey Space\n' \
             f'click {heading + 2} 3\nshow\n'
    tui = run(vega, 'tui', module, '--dump', grid, stdin=script)
    expected = [line for line in tui.stdout.splitlines() if not line.startswith('cells ')]
    got = [line for line in live.stdout.splitlines() if not line.startswith('frame ')]
    assert got == expected, (got, expected)
    return {'events': len(events), 'lines': len(got)}


def accessibility(vega, module, work):
    """The display on an accessibility bus of its own (a private session bus
    with at-spi-bus-launcher in it), walked as a screen reader walks it, with
    gdbus: the registry lists the app; its tree has the title as a heading,
    the field as a label, the list as a list box of items that know their
    place, the entry and the keys as buttons; one object at a time is
    focused (the entry while typing, then the list's focused row); keys,
    a row selected through Selection and a key's button pressed through
    Action move the focus, and the display says so in events."""
    import re, shutil, signal, struct, time
    launcher = Path('/usr/libexec/at-spi-bus-launcher')
    card, face = Path('/dev/dri/card1'), work / 'ui.vsym'
    if not (shutil.which('dbus-daemon') and shutil.which('gdbus') and launcher.exists() and card.exists() and face.exists()):
        return None
    out = subprocess.run(['dbus-daemon', '--session', '--fork', '--print-address=1', '--print-pid=1', '--nopidfile'],
                         capture_output=True, text=True, check=True).stdout.split('\n')
    session, daemon = out[0], int(out[1])
    env = dict(os.environ, DBUS_SESSION_BUS_ADDRESS=session)
    helper = subprocess.Popen([launcher, '--launch-immediately'], env=env, start_new_session=True,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    shown, monitor, keys = None, None, None
    try:
        for _ in range(100):
            found = subprocess.run(['gdbus', 'call', '--session', '--dest', 'org.a11y.Bus', '--object-path', '/org/a11y/bus',
                                    '--method', 'org.a11y.Bus.GetAddress'], capture_output=True, text=True, env=env)
            if found.returncode == 0:
                break
            time.sleep(0.05)
        a11y = re.search(r"'([^']*)'", found.stdout).group(1)
        store = work / 'access-store'
        assert run(vega, 'cli', module, '--store', store, stdin='text 61\ntext 62\ntext 63\n').returncode == 0
        fifo = work / 'access-keys'
        os.mkfifo(fifo)
        shown = subprocess.Popen([str(vega), 'display', str(module), '--store', str(store), '--symbols', str(face), '--size', '18',
                                  '--card', str(card), '--writeback', '640x480', '--out', str(work / 'access'),
                                  '--input', str(fifo), '--access'],
                                 env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        keys = open(fifo, 'wb')

        def call(dest, path, method, *args):
            result = subprocess.run(['gdbus', 'call', '--address', a11y, '--dest', dest, '--object-path', path, '--method', method,
                                     *map(str, args)], capture_output=True, text=True, env=env)
            if result.returncode:
                shown.poll()
                why = shown.stderr.read() if shown.returncode is not None else 'still running'
                raise AssertionError((path, method, result.stderr, shown.returncode, why))
            return result.stdout.strip()

        name = None
        for _ in range(100):
            apps = re.findall(r"'(:[0-9.]+)', (?:objectpath )?'([^']*)'",
                              call('org.a11y.atspi.Registry', '/org/a11y/atspi/accessible/root',
                                   'org.a11y.atspi.Accessible.GetChildren'))
            if apps:
                name, root = apps[-1]
                break
            time.sleep(0.05)
        assert name, 'the registry never listed the display'

        def item(path):
            role = re.search(r"'([^']*)'", call(name, path, 'org.a11y.atspi.Accessible.GetRoleName')).group(1)
            label = re.search(r"<'(.*)'>", call(name, path, 'org.freedesktop.DBus.Properties.Get', 'org.a11y.atspi.Accessible',
                                                'Name')).group(1)
            bits = [int(b) for b in re.findall(r'\d+', call(name, path, 'org.a11y.atspi.Accessible.GetState')
                                                .replace('uint32', ''))[:2]]
            states = {s for s, bit in (('focused', 12), ('selected', 23), ('editable', 7)) if bits[bit // 32] >> (bit % 32) & 1}
            kids = re.findall(r"'(/[^']*)'", call(name, path, 'org.a11y.atspi.Accessible.GetChildren'))
            return role, label, states, kids

        def tree(path, into):
            role, label, states, kids = item(path)
            into.append((path, role, label, states))
            for kid in kids:
                tree(kid, into)
            return into

        def focused(nodes):
            return [(role, label) for path, role, label, states in nodes if 'focused' in states]

        nodes = tree(root, [])
        roles = [(role, label) for path, role, label, states in nodes]
        assert roles[:3] == [('application', 'To do'), ('frame', 'To do'), ('heading', 'To do')], roles
        assert ('label', 'left: 3 of 3') in roles and ('list box', 'tasks_list') in roles and ('entry', 'add') in roles, roles
        items = [(label, states) for path, role, label, states in nodes if role == 'list item']
        assert [label for label, _ in items] == ['a', 'b', 'c'] and 'selected' in items[2][1], items
        buttons = {label: path for path, role, label, states in nodes if role == 'push button'}
        assert 'up (ArrowUp)' in buttons and 'toggle (Space)' in buttons, buttons
        assert focused(nodes) == [('entry', 'add')], focused(nodes)  # typing, as the surface starts
        rows = [path for path, role, label, states in nodes if role == 'list item']
        places = call(name, rows[1], 'org.a11y.atspi.Accessible.GetAttributes')
        assert "'posinset': '2'" in places and "'setsize': '3'" in places, places

        monitor = subprocess.Popen(['gdbus', 'monitor', '--address', a11y, '--dest', name], env=env,
                                   stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        time.sleep(0.3)

        def press(code):
            keys.write(struct.pack('llHHi', 0, 0, 1, code, 1) + struct.pack('llHHi', 0, 0, 0, 0, 0) +
                       struct.pack('llHHi', 0, 0, 1, code, 0) + struct.pack('llHHi', 0, 0, 0, 0, 0))
            keys.flush()

        def until(what, test):
            for _ in range(100):
                nodes = tree(root, [])
                if test(nodes):
                    return nodes
                time.sleep(0.05)
            raise AssertionError(f'{what}: {focused(nodes)}')

        press(15)  # Tab: keys go to the list
        until('Tab gives the list the focus', lambda n: focused(n) == [('list item', 'c')])
        press(103)  # ArrowUp
        until('ArrowUp moves it', lambda n: focused(n) == [('list item', 'b')])
        assert call(name, [p for p, r, l, s in nodes if r == 'list box'][0], 'org.a11y.atspi.Selection.SelectChild', 0) == '(true,)'
        until('a row selected through Selection is focused', lambda n: focused(n) == [('list item', 'a')])
        assert call(name, buttons['toggle (Space)'], 'org.a11y.atspi.Action.DoAction', 0) == '(true,)'
        nodes = until('the toggle key pressed through Action marks it done',
                      lambda n: focused(n) == [('list item', 'a, done')])
        keys.close()
        keys = None
        out, err = shown.communicate(timeout=20)
        assert shown.returncode == 0, err
        monitor.terminate()
        said = monitor.communicate(timeout=10)[0]
        events = re.findall(r"(/org/vega/[^:]*): org\.a11y\.atspi\.Event\.Object\.(\w+) \('(\w*)', (-?\d+)", said)
        moves = [(path.rsplit('/', 1)[-1], detail, int(value)) for path, member, detail, value in events
                 if member == 'StateChanged' and detail == 'focused' and int(value)]
        assert [row for row, _, _ in moves] == ['2', '1', '0'], events
        return {'objects': len(nodes), 'events': len(events)}
    finally:
        if keys:
            keys.close()
        for process in (shown, monitor):
            if process and process.poll() is None:
                process.kill()
        os.killpg(helper.pid, signal.SIGTERM)
        os.kill(daemon, signal.SIGTERM)


def hangul(vega, module):
    """Hangul typed on the keyboard (두벌식, surfaces/hangul.c): letter keys
    compose syllables in the entry line after the 한/영 key, and Enter commits
    them. Joined vowels and finals, a final moving on to a vowel after it,
    Shift's doubled consonants, a lone jamo, Backspace taking back one jamo at
    a time, and a space or digit ending a syllable, each as an item."""
    cases = [('dkssudgktpdy', '', '안녕하세요'), ('rkqtdmf', '', '값을'), ('dkswk', '', '안자'),
             ('dhkdnjs', '', '와원'), ('Rkdnjs', '', '까원'), ('rkqt', 'back', '갑'), ('gksrmf', '', '한글'),
             ('k', '', 'ㅏ'), ('dkswk', 'back', '안ㅈ'), ('dkssud 2', '', '안녕 2')]
    script = 'hangul\n' + ''.join(f'letters {keys.encode().hex()}\n' + (f'{after}\n' if after else '') + 'enter\n'
                                  for keys, after, _ in cases) + 'show\n'
    shown = run(vega, 'tui', module, '--dump', '40x16', stdin=script)
    assert shown.returncode == 0, shown.stderr
    labels = [line[1:].strip().split('  ')[0].strip() for line in shown.stdout.splitlines()
              if line[1:].startswith('  ') or line[1:].startswith('> ')]
    labels = [label.lstrip('> ').strip() for label in labels]
    assert labels[:len(cases)] == [want for _, _, want in cases], (labels, shown.stdout)
    # The syllable being composed shows on the entry line; Backspace takes it back jamo by jamo, then a letter.
    typing = run(vega, 'tui', module, '--dump', '30x5',
                 stdin=f'hangul\nletters {"dkssu".encode().hex()}\nshow\nback\nshow\nback\nback\nshow\n')
    prompts = [line for line in typing.stdout.splitlines() if line.startswith('|') and '>' in line[:6] and 'add' in line]
    assert [p.split('>', 1)[1].split('\t')[0].strip() for p in prompts] == ['안녀', '안ㄴ', ''], typing.stdout
    return {'items': len(cases)}


def node_display(vega, serve, module, work):
    """Vega in a Pure node (vega-surface.so run by pure-serve --surface): the
    node's app, which an HTTP client gave an item, is on its display, and
    recorded keys typed there (an item, Enter, Tab, Space) go to the node's
    owner. The last frame is the one the vega command draws for the same
    state and keys, it came back from the display controller as drawn, and
    the node's HTTP views show what was typed."""
    import http.client
    import select
    import shutil
    import signal
    import struct
    import time
    card, face, library = Path('/dev/dri/card1'), work / 'ui.vsym', vega.parent / 'vega-surface.so'
    if not card.exists() or not face.exists() or not library.exists():
        return None
    store = work / 'node-store'

    def start(*extra):
        """The node, its port, and what it printed after the line that tells it."""
        process = subprocess.Popen([str(a) for a in (serve, '--module', module, '--store', store, '--port', 0, *extra)],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        printed, deadline = '', time.time() + 10
        while '\n' not in printed and time.time() < deadline:
            if select.select([process.stdout], [], [], 1)[0]:
                printed += os.read(process.stdout.fileno(), 65536).decode()
        assert '\n' in printed, 'node did not start'
        first, rest = printed.split('\n', 1)
        return process, json.loads(first)['port'], rest

    def post(port, path, body):
        connection = http.client.HTTPConnection('127.0.0.1', port, timeout=10)
        connection.request('POST', path, body)
        status = connection.getresponse().status
        connection.close()
        return status

    node, port, _ = start()
    try:
        assert post(port, '/inputs/add', b'eggs') == 200
    finally:
        node.send_signal(signal.SIGTERM)
        node.communicate(timeout=30)
    before = work / 'node-store-before'
    shutil.copytree(store, before)
    codes = {'m': 50, 'i': 23, 'l': 38, 'k': 37, 'enter': 28, 'tab': 15, 'space': 57}
    events = []  # (type, code, value): EV_KEY 1, EV_SYN 0
    for name in ('m', 'i', 'l', 'k', 'enter', 'tab', 'space'):
        events.extend([(1, codes[name], 1), (0, 0, 0), (1, codes[name], 0), (0, 0, 0)])
    recording = work / 'node-events.bin'
    recording.write_bytes(b''.join(struct.pack('llHHi', 0, 0, *event) for event in events))
    node, port, printed = start('--surface', library, '--', 'display', '--symbols', face, '--size', 18, '--card', card,
                                '--writeback', '640x480', '--out', work / 'node', '--input', recording)
    lines = []
    try:
        deadline = time.time() + 30
        while '\nframe ' not in '\n' + printed and time.time() < deadline:
            ready, _, _ = select.select([node.stdout], [], [], 1)
            if ready:
                printed += os.read(node.stdout.fileno(), 65536).decode()
        lines = printed.splitlines()
        assert any(line.startswith('frame ') for line in lines), lines
        left = http.client.HTTPConnection('127.0.0.1', port, timeout=10)
        left.request('GET', '/views/left')
        shown = left.getresponse().read().decode()
        left.close()
    finally:
        node.send_signal(signal.SIGTERM)
        _, error = node.communicate(timeout=30)
    if node.returncode == 1 and b'writeback' in error:
        return None
    assert node.returncode == 0 and b'"returned":0' in error, error[-800:]
    assert shown == '1 of 2', shown  # eggs to do, milk done
    frame = next(line.split() for line in lines if line.startswith('frame '))
    assert frame[3] == '0', frame  # came back as drawn
    script = f'type {"milk".encode().hex()}\nenter\ntab\nkey Space\nshow\n'
    tui = run(vega, 'tui', module, '--store', before, '--dump', frame[5], stdin=script)
    expected = [line for line in tui.stdout.splitlines() if not line.startswith('cells ')]
    got = [line for line in lines if not line.startswith('frame ')]
    assert got == expected, (got, expected)
    return {'events': len(events), 'lines': len(got)}


def messenger(vega, serve, module, work):
    """Two nodes of examples/messenger.aup, Alice's and Bob's, each with a key
    of its own, sealing to the other's and taking lines only from the other
    (pure-serve --key, --send, --seal, --sender), each showing its app
    through Vega's pixels in the node. Lines typed on Alice's display
    (recorded keys: one in English, one in Hangul after the 한/영 key) reach
    Bob's node sealed and are drawn on his; Bob's answer, posted by a
    program, is drawn on hers. Each last frame is the one
    the vega command draws from that node's store, and the relay between the
    nodes saw no line's text nor a field's name, only seals made in one
    session each and naming the sender's key."""
    import http.client
    import select
    import shutil
    import signal
    import struct
    import threading
    import time
    from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
    face, library = work / 'ui.vsym', vega.parent / 'vega-surface.so'
    if not face.exists() or not library.exists():
        return None
    nodes = {}  # name: (process, port)
    seen = []   # (path, headers, body) relayed

    class Relay(BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'

        def do_POST(self):
            body = self.rfile.read(int(self.headers['Content-Length']))
            to, _, path = self.path[1:].partition('/')
            headers = {k: v for k, v in self.headers.items() if k.lower().startswith('x-pure-') or
                       k.lower() in ('content-type', 'idempotency-key')}
            seen.append((self.path, headers, body))
            connection = http.client.HTTPConnection('127.0.0.1', nodes[to][1], timeout=10)
            connection.request('POST', '/' + path, body, headers)
            answer = connection.getresponse()
            data = answer.read()
            connection.close()
            self.send_response(answer.status)
            for k, v in answer.getheaders():
                if k.lower().startswith('x-pure-'):
                    self.send_header(k, v)
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def log_message(self, *args):
            pass

    def start(name, *extra):
        process = subprocess.Popen([str(a) for a in (serve, '--module', module, '--store', work / f'{name}-store',
                                                     '--port', 0, '--key', work / f'{name}.key', '--name', name, *extra)],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        printed, deadline = '', time.time() + 10
        while '\n' not in printed and time.time() < deadline:
            if select.select([process.stdout], [], [], 1)[0]:
                printed += os.read(process.stdout.fileno(), 65536).decode()
        assert '\n' in printed, 'node did not start'
        first, rest = printed.split('\n', 1)
        nodes[name] = (process, json.loads(first)['port'])
        return json.loads(first)['public_key'], rest

    def stop(name):
        process = nodes[name][0]
        process.send_signal(signal.SIGTERM)
        _, error = process.communicate(timeout=30)
        assert process.returncode == 0, error[-800:]
        return error

    def call(name, method, path, body=None):
        connection = http.client.HTTPConnection('127.0.0.1', nodes[name][1], timeout=10)
        connection.request(method, path, body)
        answer = connection.getresponse()
        data = answer.read()
        connection.close()
        return answer.status, data.decode()

    def wait_for(condition, seconds=20):
        deadline = time.time() + seconds
        while not condition() and time.time() < deadline:
            time.sleep(0.05)
        return condition()

    relay = ThreadingHTTPServer(('127.0.0.1', 0), Relay)
    threading.Thread(target=relay.serve_forever, daemon=True).start()
    keys = {}
    try:
        # Each node's key made, its name set (by a program: a message of one text field takes the body), before they meet.
        for name in ('alice', 'bob'):
            keys[name], _ = start(name)
            assert call(name, 'POST', '/inputs/rename', name.encode())[0] == 200
            stop(name)
        other = {'alice': 'bob', 'bob': 'alice'}
        wiring = {name: ['--send', f'said=http://127.0.0.1:{relay.server_address[1]}/{other[name]}/inputs/heard',
                         '--seal', f'said={keys[other[name]]}', '--sender', f'{other[name]}.said={keys[other[name]]}',
                         '--surface', library, '--', 'pixels', '--symbols', face, '--size', 16, '--grid', '40x8',
                         '--out', work / f'{name}-frame'] for name in ('alice', 'bob')}
        start('bob', *wiring['bob'])
        codes = {'h': 35, 'e': 18, 'l': 38, 'o': 24, 'b': 48, 'space': 57, 'enter': 28, 'lang1': 122, 'd': 32,
                 'k': 37, 's': 31, 'u': 22}
        events = []
        for name in ('h', 'e', 'l', 'l', 'o', 'space', 'b', 'o', 'b', 'enter', 'lang1', 'd', 'k', 's', 's', 'u', 'd',
                     'enter'):
            events.extend([(1, codes[name], 1), (0, 0, 0), (1, codes[name], 0), (0, 0, 0)])
        recording = work / 'alice-events.bin'
        recording.write_bytes(b''.join(struct.pack('llHHi', 0, 0, *event) for event in events))
        _, printed = start('alice', *wiring['alice'], '--input', recording)
        process = nodes['alice'][0]
        deadline = time.time() + 20
        while '\nframe ' not in '\n' + printed and time.time() < deadline:
            if select.select([process.stdout], [], [], 1)[0]:
                printed += os.read(process.stdout.fileno(), 65536).decode()
        assert 'hello bob' in printed and '안녕' in printed and '\nframe ' in '\n' + printed, printed
        assert wait_for(lambda: call('bob', 'GET', '/views/title')[1] == 'bob: 2 lines'), call('bob', 'GET', '/views/title')
        assert call('bob', 'POST', '/inputs/say', b'hi alice')[0] == 200
        assert wait_for(lambda: call('alice', 'GET', '/views/title')[1] == 'alice: 3 lines')
        drawn = {}
        for name in ('alice', 'bob'):
            # Its surface draws after each save: the last frame shows the last save.
            frames = work / f'{name}-frame'
            count = lambda: len(list(work.glob(f'{name}-frame-*.ppm')))
            settled = count()
            time.sleep(0.3)
            assert count() == settled
            drawn[name] = max(work.glob(f'{name}-frame-*.ppm'), key=lambda path: int(path.stem.rsplit('-', 1)[1]))
            error = stop(name)
            assert b'"returned":0' in error, error[-800:]
        for name in ('alice', 'bob'):
            copy = work / f'{name}-copy'
            shutil.copytree(work / f'{name}-store', copy)
            expected = run(vega, 'pixels', module, '--store', copy, '--symbols', face, '--size', 16, '--grid', '40x8',
                           '--out', work / f'{name}-expected', stdin='show\n')
            assert expected.returncode == 0, expected.stderr
            assert all(line in expected.stdout for line in ('hello bob', '안녕', 'hi alice')), expected.stdout
            assert drawn[name].read_bytes() == (work / f'{name}-expected-1.ppm').read_bytes(), name
    finally:
        for process, _ in nodes.values():
            if process.poll() is None:
                process.kill()
                process.wait()
        relay.shutdown()
    assert seen and all(len(headers['X-Pure-Seal'].split()) == 4 for _, headers, _ in seen), seen[:1]
    assert not any(word in body for _, _, body in seen
                   for word in (b'hello', '안녕'.encode(), b'hi alice', b'body', b'from'))
    sessions = {(headers['X-Pure-Source'], headers['X-Pure-Seal'].split()[1]) for _, headers, _ in seen}
    assert len(sessions) == 2, sessions  # one session each way
    return {'relayed': len(seen), 'lines': 3}


def width_of(character):
    return width(character) or 0



def shared_board(serve, module, work):
    """Two nodes of examples/shared-board.aup, Alice's and Bob's, each with a
    key of its own, linked both ways over pure:// (sealed, each taking the
    other as its sender). Cards added, moved and given on either reach the
    other, and both boards end the same. Then Bob's node is cut off (it
    listens and sends where no one is) while both give one card to someone,
    Bob first and Alice after, and Bob adds a card; joined again, both
    boards take Alice's, the later change, and Bob's card."""
    import http.client
    import select
    import signal
    import socket
    import time
    nodes, keys = {}, {}

    def udp_port():
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.bind(('127.0.0.1', 0))
            return probe.getsockname()[1]

    ports = {'alice': udp_port(), 'bob': udp_port()}

    def launch(args):
        process = subprocess.Popen([str(a) for a in args], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        ready, _, _ = select.select([process.stdout], [], [], 10)
        line = process.stdout.readline() if ready else b''
        assert line, 'node did not start'
        return process, json.loads(line)

    def stop(name):
        process = nodes.pop(name)[0]
        process.send_signal(signal.SIGTERM)
        _, error = process.communicate(timeout=30)
        assert process.returncode == 0, error[-800:]

    for name in ('alice', 'bob'):  # the keys first
        process, first = launch([serve, '--module', module, '--store', work / f'board-{name}', '--port', 0,
                                 '--key', work / f'board-{name}.key'])
        keys[name] = first['public_key']
        nodes[name] = (process, 0)
        stop(name)

    def start(name, cut=False):
        other = 'bob' if name == 'alice' else 'alice'
        listen, to = (udp_port(), udp_port()) if cut else (ports[name], ports[other])
        process, first = launch([serve, '--module', module, '--store', work / f'board-{name}', '--port', 0,
                                 '--key', work / f'board-{name}.key', '--name', name, '--link-port', listen,
                                 '--send', f'changed=pure://127.0.0.1:{to}/remote', '--seal', f'changed={keys[other]}',
                                 '--sender', f'{other}.changed={keys[other]}'])
        assert first['name'] == name, first
        nodes[name] = (process, first['port'])

    def call(name, path, body=None):
        connection = http.client.HTTPConnection('127.0.0.1', nodes[name][1], timeout=10)
        headers = {}
        if body is not None:
            data = body.encode() if isinstance(body, str) else json.dumps(body).encode()
            headers = {'Content-Length': str(len(data)),
                       'Content-Type': 'text/plain' if isinstance(body, str) else 'application/json'}
        connection.request('POST' if body is not None else 'GET', path, data if body is not None else None, headers)
        answer = connection.getresponse()
        text = answer.read().decode()
        connection.close()
        return answer.status, text

    def board(name):
        return sorted((c['label'], c['badge']) for c in json.loads(call(name, '/views/cards_list')[1])['items'])

    def agree(expected=None, seconds=20):
        deadline = time.time() + seconds
        while time.time() < deadline:
            a, b = board('alice'), board('bob')
            if a == b and (expected is None or a == expected):
                return a
            time.sleep(0.05)
        raise AssertionError((board('alice'), board('bob'), expected))

    try:
        for name in ('alice', 'bob'):
            start(name)
            assert call(name, '/inputs/name', {'name': name})[0] == 200
        for title in ('plan the week', 'write the tests'):
            assert call('alice', '/inputs/add', title)[0] == 200
        assert call('bob', '/inputs/add', 'fix the build')[0] == 200
        assert call('alice', '/inputs/forward', '')[0] == 200  # write the tests: doing
        agree(seconds=20)
        assert call('bob', '/inputs/assign', {'title': 'plan the week', 'who': 'bob'})[0] == 200
        together = agree([('fix the build', 'to do'), ('plan the week', 'bob, to do'), ('write the tests', 'doing')])
        stop('bob')
        start('bob', cut=True)
        assert call('bob', '/inputs/assign', {'title': 'fix the build', 'who': 'bob'})[0] == 200
        time.sleep(0.05)  # Alice's is the later change
        assert call('alice', '/inputs/assign', {'title': 'fix the build', 'who': 'alice'})[0] == 200
        assert call('bob', '/inputs/add', 'made while cut off')[0] == 200
        time.sleep(0.5)
        assert ('fix the build', 'bob, to do') in board('bob') and ('fix the build', 'alice, to do') in board('alice')
        stop('bob')
        start('bob')
        joined = agree([('fix the build', 'alice, to do'), ('made while cut off', 'to do'), ('plan the week', 'bob, to do'),
                        ('write the tests', 'doing')])
    finally:
        for name in list(nodes):
            stop(name)
    return {'cards': len(together), 'after_joining': len(joined)}

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--vega', type=Path, required=True)
    parser.add_argument('--aup', type=Path, required=True)
    parser.add_argument('--serve', type=Path, required=True)
    parser.add_argument('--clock-back', type=Path, help="Pure's build/clock-back.so: the wall clock set back")
    parser.add_argument('--symbols', type=Path)
    parser.add_argument('--seeds', type=int, default=8)
    parser.add_argument('--rounds', type=int, default=3)
    args = parser.parse_args()
    vega, aup = args.vega.resolve(), args.aup.resolve()
    report = {}
    with tempfile.TemporaryDirectory(prefix='vega-check-') as directory:
        work = Path(directory)
        env = dict(os.environ, AUP_PATH=str(ROOT / 'lib'))
        modules = {}
        for name in ('todo', 'scores', 'today', 'stopwatch', 'board', 'messenger', 'shared-board'):
            modules[name] = work / f'{name}.pmod'
            built = run(aup, 'build', ROOT / 'examples' / f'{name}.aup', '-o', modules[name], env=env)
            assert built.returncode == 0, built.stderr
        report['cli_and_say_lines'], report['failed_events_spoken'] = \
            cli_and_say(vega, modules['todo'], range(args.seeds))
        report['tui_frames'] = tui(vega, modules['todo'], range(args.seeds), [(40, 12), (24, 9), (80, 30), (16, 6)])
        report['table_lines'] = scores(vega, modules['scores'])
        report['timed_lines'], report['ticks_heard'] = timing(vega, modules['stopwatch'])
        if args.clock_back:
            report['beats_with_clock_set_back'] = clock_set_back(vega, aup, work, args.clock_back.resolve())
        report['board_rows_across_hosts'] = hosts(vega, args.serve.resolve(), modules['board'], work)
        report['hot_swap_lines'] = hot_swap(vega, aup, work)
        report['split_list_lines'] = split(vega, modules['today'], work, range(args.seeds))
        report['painted_frames'], report['first_paint_bytes'], report['later_paint_bytes'] = \
            painting(vega, modules['todo'], range(args.seeds), [(40, 12), (24, 9), (80, 30)])
        report['web_screens'] = web(vega, modules['todo'], work, range(args.seeds))
        report['browser_steps'] = browser(vega, modules['todo'], work)
        report['interactive_sessions'] = interactive(vega, modules['todo']) + \
            interactive(vega, modules['todo'], work / 'interactive-store') + \
            interactive(vega, modules['todo'], cut=True)
        report['stored_surface_lines'] = stored_surfaces(vega, modules['todo'], work, range(args.seeds))
        report['hangul'] = hangul(vega, modules['todo'])
        report['convention_findings'] = conventions(vega, aup, work, modules)
        report['examples'], report['robustness_runs'] = robustness(vega, aup, work, args.rounds)
        if args.symbols:
            report['symbols'] = symbols(args.symbols.resolve(), work)
            report['pixels'] = pixels(vega, args.symbols.resolve(), modules['todo'], work)
            report['typeset'] = typeset(vega, args.symbols.resolve(), modules['todo'], work)
            report['display'] = display(vega, modules['todo'], work)
            report['accessibility'] = accessibility(vega, modules['todo'], work)
            report['live_display'] = live_display(vega, modules['todo'], work)
            report['node_display'] = node_display(vega, args.serve.resolve(), modules['todo'], work)
            report['messenger'] = messenger(vega, args.serve.resolve(), modules['messenger'], work)
        report['shared_board'] = shared_board(args.serve.resolve(), modules['shared-board'], work)
    print(json.dumps(report))


if __name__ == '__main__':
    sys.exit(main())
