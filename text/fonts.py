#!/usr/bin/env python3
"""Every font here against HarfBuzz, on random runs:

    python3 text/fonts.py VEGA_SYMBOLS [--runs N] [--only TEXT] [FONT...]

The fonts given, else every font fontconfig lists (of a collection, its
Korean face: CJK collections are why there are collections here). Each is
built (those with outlines) into a symbol file with its characters in the ranges below and
shaping for each script it has ten letters of (and Common); the build
refuses a file whose shaping differs from HarfBuzz's (vega-symbols build
--runs). A line per font; the last is JSON: fonts, plans, runs that
differed, fonts that failed. Needs fontTools; exits 1 on a difference.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile

from fontTools.ttLib import TTCollection, TTFont

RANGES = [(0x20, 0x24F), (0x250, 0x2FF), (0x300, 0x36F), (0x370, 0x52F), (0x590, 0x5FF), (0x600, 0x6FF),
          (0x750, 0x77F), (0x1E00, 0x1FFF), (0x2000, 0x206F), (0x20A0, 0x20CF), (0xFB00, 0xFB4F), (0xFE70, 0xFEFF),
          (0x1100, 0x11FF), (0x302E, 0x302F), (0x3130, 0x318F), (0xA960, 0xA97F), (0xAC00, 0xD7A3), (0xD7B0, 0xD7FF),
          (0x900, 0xD7F), (0x1CD0, 0x1CFF), (0xA8E0, 0xA8FF), (0x25CC, 0x25CC), (0x1000, 0x109F), (0xA9E0, 0xA9FF),
          (0xAA60, 0xAA7F), (0x1780, 0x17FF), (0x19E0, 0x19FF), (0xD80, 0xDFF), (0xF00, 0xFFF), (0xA980, 0xA9DF),
          (0x1B00, 0x1B7F), (0x1800, 0x18AF), (0xE00, 0xEFF), (0x700, 0x74F), (0x860, 0x86F), (0xFB50, 0xFDFF),
          (0xFE00, 0xFE0F)]
LETTERS = {'Latn': (0x41, 0x7A), 'Grek': (0x391, 0x3C9), 'Cyrl': (0x410, 0x44F), 'Hebr': (0x5D0, 0x5EA),
           'Arab': (0x621, 0x64A), 'Hang': (0xAC00, 0xD7A3), 'Deva': (0x915, 0x939), 'Beng': (0x995, 0x9B9),
           'Guru': (0xA15, 0xA39), 'Gujr': (0xA95, 0xAB9), 'Orya': (0xB15, 0xB39), 'Taml': (0xB95, 0xBB9),
           'Telu': (0xC15, 0xC39), 'Knda': (0xC95, 0xCB9), 'Mlym': (0xD15, 0xD39), 'Khmr': (0x1780, 0x17B3),
           'Mymr': (0x1000, 0x102A), 'Sinh': (0xD9A, 0xDC6), 'Tibt': (0xF40, 0xF6C), 'Java': (0xA98F, 0xA9B2),
           'Bali': (0x1B13, 0x1B33), 'Mong': (0x1820, 0x1878), 'Thai': (0xE01, 0xE2E), 'Laoo': (0xE81, 0xEAE),
           'Syrc': (0x710, 0x72C)}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('tool')
    parser.add_argument('fonts', nargs='*')
    parser.add_argument('--runs', default='500')
    parser.add_argument('--only', default='')
    args = parser.parse_args()
    fonts = args.fonts or sorted(set(subprocess.run(['fc-list', '--format', '%{file}\n'], capture_output=True,
                                                    text=True).stdout.split()))
    total = {'fonts': 0, 'plans': 0, 'differences': 0, 'failed': 0}
    with tempfile.TemporaryDirectory(prefix='vega-fonts-') as work:
        for path in fonts:
            if args.only not in path:
                continue
            face = 0
            try:
                if path.endswith('.ttc'):
                    names = [f['name'].getDebugName(4) or '' for f in TTCollection(path, lazy=True).fonts]
                    korean = [i for i, n in enumerate(names) if n.endswith(' KR')]
                    if not korean:
                        continue
                    face = korean[0]
                font = TTFont(path, fontNumber=face, lazy=True)
                if not any(tag in font for tag in ('glyf', 'CFF ', 'CFF2')):
                    continue  # bitmaps alone (color emoji): nothing to draw from
                cmap = font.getBestCmap() or {}
            except Exception as e:  # not a font fontTools reads
                print('skip', path, e)
                continue
            codes = sorted(c for c in cmap if any(a <= c <= b for a, b in RANGES))
            if not codes:
                continue
            spans, start = [], codes[0]
            for a, b in zip(codes, codes[1:] + [None]):
                if b != a + 1:
                    spans.append(f'{start:#x}-{a:#x}' if a != start else f'{a:#x}')
                    start = b
            scripts = [s for s, (a, b) in LETTERS.items() if sum(1 for c in codes if a <= c <= b) >= 10] + ['Zyyy']
            built = subprocess.run([args.tool, 'build', '--font', path] + (['--face', str(face)] if face else []) +
                                   ['--codes', ','.join(spans), '--shape', ','.join(scripts), '--runs', args.runs,
                                    '-o', os.path.join(work, 'font.vsym')], capture_output=True, text=True)
            plans = [json.loads(line) for line in built.stdout.splitlines() if line.startswith('{"script"')]
            differences = sum(p['differences'] for p in plans)
            status = 'ok' if built.returncode == 0 else 'FAIL'
            why = (built.stderr.strip().splitlines() or [''])[-1][:150] if built.returncode else ''
            print(f'{status:4} {os.path.basename(path):40} {",".join(scripts):30} plans={len(plans):2} '
                  f'differences={differences}', why, flush=True)
            total['fonts'] += 1
            total['plans'] += len(plans)
            total['differences'] += differences
            total['failed'] += built.returncode != 0 and not differences
    print(json.dumps(total))
    return 1 if total['differences'] or total['failed'] else 0


if __name__ == '__main__':
    sys.exit(main())
