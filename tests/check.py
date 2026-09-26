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
7. Painting: the bytes the TUI sends, replayed by a small terminal emulator,
   give the very frame the dump shows, while sending only what changed.
"""
import argparse
import json
import os
import random
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
        """Returns False when the event fails (a missing item)."""
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

    @staticmethod
    def badge(task):
        return 'done' if task['done'] else 'urgent' if task['urgent'] else ''

    @staticmethod
    def tone(task):
        return 'muted' if task['done'] else 'danger' if task['urgent'] else 'normal'

    def cli(self):
        lines = ['To do', f'left: {len(self.tasks)} tasks', f'tasks_list ({len(self.tasks)})']
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
        lines = ['To do.', f'left: {n} tasks.', f'tasks_list, {n} item{"" if n == 1 else "s"}.']
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
            if rng.random() < 0.25:
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
                if rng.random() < 0.3:
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
            model, script, states = Todo(), [], []
            for _ in range(steps):
                if rng.random() < 0.3:
                    word = rng.choice(WORDS)
                    script.append('text ' + (word.encode().hex() or '-'))
                    model.event('add', word)
                else:
                    name = rng.choice(list(KEYS))
                    script.append(f'key {KEYS[name]}')
                    model.event(name)
                script.append('show')
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
    return len(out)


def interactive(vega, module, store=None):
    """The terminal surface on a real pseudo-terminal: typed text commits
    with Enter, arrow keys and letters reach the app after Tab, Ctrl-C leaves
    and restores the terminal."""
    import fcntl, pty, select, struct, termios, time
    pid, fd = pty.fork()
    if pid == 0:
        os.execv(str(vega), [str(vega), 'tui', str(module)] + (['--store', str(store)] if store else []))
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack('HHHH', 10, 40, 0, 0))
    os.kill(pid, 28)  # SIGWINCH: take the size

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
    for keys in (b'milk\r', 'b\u00e9ton\r'.encode(), b'\t', b'\x1b[A', b' ', b'u'):
        os.write(fd, keys)
        screen += read_for(0.2)
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
    final = '\n'.join(shown(grid))
    assert 'milk' in final and 'béton' in final and '(2)' in final and 'done' in final, final
    if store:  # what was typed is still there in the next session
        again = run(vega, 'cli', module, '--store', store, stdin='show\n').stdout.splitlines()
        assert again == ['To do', 'left: 2 tasks', 'tasks_list (2)', '> milk [done] (muted)', '  béton'], again
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
                else: script.append('key ' + rng.choice(keys))
            text = '\n'.join(script) + '\nshow\n'
            size = rng.choice(['80x24', '12x5', '1x1', '200x3'])
            for surface in (['cli'], ['say'], ['tui', '--dump', size], ['tui', '--paint', size]):
                result = run(vega, surface[0], module, *surface[1:], stdin=text)
                assert result.returncode == 0, (path, surface, result.stderr[-400:])
                runs += 1
    return len(examples), runs


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--vega', type=Path, required=True)
    parser.add_argument('--aup', type=Path, required=True)
    parser.add_argument('--seeds', type=int, default=8)
    parser.add_argument('--rounds', type=int, default=3)
    args = parser.parse_args()
    vega, aup = args.vega.resolve(), args.aup.resolve()
    report = {}
    with tempfile.TemporaryDirectory(prefix='vega-check-') as directory:
        work = Path(directory)
        env = dict(os.environ, AUP_PATH=str(ROOT / 'lib'))
        modules = {}
        for name in ('todo', 'scores'):
            modules[name] = work / f'{name}.pmod'
            built = run(aup, 'build', ROOT / 'examples' / f'{name}.aup', '-o', modules[name], env=env)
            assert built.returncode == 0, built.stderr
        report['cli_and_say_lines'], report['failed_events_spoken'] = \
            cli_and_say(vega, modules['todo'], range(args.seeds))
        report['tui_frames'] = tui(vega, modules['todo'], range(args.seeds), [(40, 12), (24, 9), (80, 30), (16, 6)])
        report['table_lines'] = scores(vega, modules['scores'])
        report['painted_frames'], report['first_paint_bytes'], report['later_paint_bytes'] = \
            painting(vega, modules['todo'], range(args.seeds), [(40, 12), (24, 9), (80, 30)])
        report['interactive_sessions'] = interactive(vega, modules['todo']) + \
            interactive(vega, modules['todo'], work / 'interactive-store')
        report['stored_surface_lines'] = stored_surfaces(vega, modules['todo'], work, range(args.seeds))
        report['convention_findings'] = conventions(vega, aup, work, modules)
        report['examples'], report['robustness_runs'] = robustness(vega, aup, work, args.rounds)
    print(json.dumps(report))


if __name__ == '__main__':
    sys.exit(main())
