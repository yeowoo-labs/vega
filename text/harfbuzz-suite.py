#!/usr/bin/env python3
"""HarfBuzz's own shaping tests, as cases for Vega's shaping:

    python3 text/harfbuzz-suite.py HARFBUZZ_SRC VEGA_SYMBOLS [--known FILE] [--show N]

HARFBUZZ_SRC is a HarfBuzz release's source; its test/shape/data holds
fonts and inputs (in-house, text-rendering-tests, aots), many of them added
when a font or a shaper once went wrong. Their expected outputs are
HarfBuzz's under options Vega has no counterpart for (font functions,
sizes, output formats), so the oracle is HarfBuzz itself run on the same
font and input (vega-symbols shape-cases, linked with the HarfBuzz the
build uses): each case must shape the same, glyph for glyph and unit for
unit. A case asking for what Vega does not do (user features, variations,
a language, context, cluster levels, buffer flags, vertical text) is left
out and counted by why.

Cases known to differ are listed in the known file (text/harfbuzz-suite.known:
an ID or a pattern of IDs a line, then # and why). The run fails on any
other difference, and names known ones that now agree. It prints one JSON
line: what was compared, what was left out and why.
"""
import argparse
import fnmatch
import json
import shlex
import subprocess
import sys
from collections import Counter, defaultdict
from pathlib import Path

# Options that change only what hb-shape prints, or how it gets a font's
# metrics (the oracle uses HarfBuzz's own, as Vega's build does), or buffer
# flags about default ignorables: the input still tells.
IGNORED = {'--font-funcs', '--font-size', '--ned', '--no-glyph-names', '--no-positions', '--no-clusters',
           '--no-advances', '--single-par', '--show-flags', '--show-extents', '--show-text', '--show-unicode',
           '--show-line-num', '--unsafe-to-concat', '--safe-to-insert-tatweel', '--verify', '--utf8-clusters',
           '--normalize-glyphs', '--remove-default-ignorables', '--preserve-default-ignorables', '--glyphs',
           '--no-glyph-extents'}
VALUED = {'--font-funcs', '--font-size', '--features', '--variations', '--direction', '--language', '--script',
          '--unicodes-before', '--unicodes-after', '--cluster-level', '--shaper', '--shapers', '--font-ptem',
          '--face-index', '--not-found-variation-selector-glyph', '--invisible-glyph', '--named-instance',
          '--font-slant', '--font-bold', '--font-grade'}


def cases(root):
    """(id, font, face, script, direction, codes) of each case, or (id, None, why) for one left out."""
    data = root / 'test' / 'shape' / 'data'
    for tests in sorted(data.glob('*/tests/*.tests')):
        for number, line in enumerate(tests.read_text(encoding='utf-8').splitlines(), 1):
            if not line.strip() or line.startswith('#'):
                continue
            case = f'{tests.parent.parent.name}/{tests.name}:{number}'
            fields = line.split(';')
            if len(fields) < 4:
                yield case, None, 'malformed'
                continue
            font, options, text = fields[0], fields[1], fields[2]
            tokens, why = shlex.split(options), None
            face, script, direction = 0, '-', '-'
            i = 0
            while i < len(tokens) and not why:
                name, _, value = tokens[i].partition('=')
                if name in VALUED and not _:
                    i += 1
                    value = tokens[i] if i < len(tokens) else ''
                i += 1
                if name in IGNORED:
                    continue
                if name == '--direction':
                    direction = {'l': 'ltr', 'ltr': 'ltr', 'r': 'rtl', 'rtl': 'rtl'}.get(value.lower())
                    why = None if direction else 'vertical text'
                elif name == '--script':
                    script = value
                elif name == '--face-index':
                    face = int(value)
                else:
                    why = {'--features': 'user features', '--variations': 'variations', '--language': 'a language',
                           '--unicodes-before': 'context', '--unicodes-after': 'context',
                           '--cluster-level': 'cluster levels', '--bot': 'buffer flags', '--eot': 'buffer flags',
                           '--font-ptem': 'point size (trak)'}.get(name, f'option {name}')
            if why:
                yield case, None, why
                continue
            # as hb-shape's --unicodes reads them: hex, U+ or 0x before or not
            codes = [int(part[2:] if part[:2].lower() in ('u+', '0x') else part, 16)
                     for part in text.replace(',', ' ').split()]
            if not codes:
                yield case, None, 'no text'
                continue
            yield case, (tests.parent / font).resolve(), face, script, direction, codes


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('harfbuzz', type=Path)
    parser.add_argument('tool', type=Path)
    parser.add_argument('--known', type=Path, default=Path(__file__).with_name('harfbuzz-suite.known'))
    parser.add_argument('--show', type=int, default=10)
    args = parser.parse_args()
    known = []
    if args.known.exists():
        for line in args.known.read_text(encoding='utf-8').splitlines():
            pattern = line.split('#', 1)[0].strip()
            if pattern:
                known.append(pattern)
    left_out, by_font = Counter(), defaultdict(list)
    for case in cases(args.harfbuzz):
        if case[1] is None:
            left_out[case[2]] += 1
        else:
            by_font[(case[1], case[2])].append(case)
    outcome, details = {}, {}
    for (font, face), group in sorted(by_font.items()):
        if not font.exists():  # a system font of another system (macOS)
            left_out['font not here'] += len(group)
            continue
        lines = ''.join(f'{case[0]} {case[3]} {case[4]} {",".join(f"{c:04X}" for c in case[5])}\n' for case in group)
        command = [str(args.tool), 'shape-cases', str(font)] + (['--face', str(face)] if face else [])
        run = subprocess.run(command, input=lines, capture_output=True, text=True, timeout=600)
        if run.returncode:
            for case in group:
                outcome[case[0]] = f'tool failed: {run.stderr.strip()[:80]}'
            continue
        last = None
        for line in run.stdout.splitlines():
            if line.startswith('  '):
                details[last] = details.get(last, '') + line + '\n'
                continue
            case, verdict = line.split(' ', 1)
            outcome[case], last = verdict, case
        for case in group:
            outcome.setdefault(case[0], 'no answer')
    counts = Counter(verdict.split(' ', 1)[0] for verdict in outcome.values())
    is_known = lambda case: any(fnmatch.fnmatchcase(case, pattern) for pattern in known)
    unexpected = sorted(case for case, verdict in outcome.items() if verdict != 'same' and not is_known(case))
    # a listed case, or pattern, all of whose cases now agree
    now_same = sorted(pattern for pattern in known
                      if any(fnmatch.fnmatchcase(case, pattern) for case in outcome) and
                      all(verdict == 'same' for case, verdict in outcome.items() if fnmatch.fnmatchcase(case, pattern)))
    for case in unexpected[:args.show]:
        print(f'{case}: {outcome[case]}', file=sys.stderr)
        print(details.get(case, ''), end='', file=sys.stderr)
    for case in now_same:
        print(f'{case}: known to differ, now the same', file=sys.stderr)
    print(json.dumps({'cases': len(outcome) + sum(left_out.values()), 'compared': len(outcome), 'same': counts['same'],
                      'differ': counts['differs'], 'failed': counts['failed'], 'refused': counts['refused'],
                      'other': len(outcome) - counts['same'] - counts['differs'] - counts['failed'] - counts['refused'],
                      'unexpected': len(unexpected), 'known_now_same': len(now_same),
                      'left_out': dict(left_out.most_common())}))
    return 1 if unexpected else 0


if __name__ == '__main__':
    sys.exit(main())
