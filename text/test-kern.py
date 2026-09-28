#!/usr/bin/env python3
"""The kern table's class subtables against HarfBuzz, where no font here has
them:

    python3 text/test-kern.py VEGA_SYMBOLS [FONT.ttf]

Fonts made from DejaVu Sans without its GPOS, their kern table a format 3
subtable and a format 2 one in every order (and two of either), with what
HarfBuzz's reading of them turns on: class value 1 (out of bounds),
glyphs outside a class table (a digest's false positive takes class 0),
offsets below the kerning array, odd ones, values past the subtable's end
(the last subtable may read on to the table's) and past the table's, class
and index values past their counts. Each is built with vega-symbols, which
refuses a font whose shaping differs from HarfBuzz's on random runs.
Needs fontTools; prints one JSON line, exits 1 on a difference.
"""
import json
import random
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

from fontTools.ttLib import TTFont, newTable


def subtable(format, body):
    return struct.pack('>HHBB', 0, 6 + len(body), format, 1) + body  # horizontal


def format3(rng, glyphs):
    lefts, rights, values = 7, 9, 40
    body = struct.pack('>HBBBB', glyphs, values, lefts, rights, 0)
    body += b''.join(struct.pack('>h', rng.randrange(-150, 150)) for _ in range(values))
    body += bytes(rng.randrange(lefts + 1) for _ in range(glyphs))  # some past the count
    body += bytes(rng.randrange(rights + 1) for _ in range(glyphs))
    body += bytes(rng.randrange(values + 3) for _ in range(lefts * rights))
    return subtable(3, body)


def format2(rng, latin):
    rows, columns = 6, 8
    width = 2 * columns
    first_left, first_right, left_count, right_count = latin[5], latin[3], 60, 70
    left_at = 6 + 8
    right_at = left_at + 4 + 2 * left_count
    array = right_at + 4 + 2 * right_count

    def left():
        k = rng.randrange(20)
        return (1 if k == 0 else rng.randrange(array) if k == 1 else array + rows * width + 64 if k == 2 else
                array + rng.randrange(rows) * width + (k == 3))

    def right():
        k = rng.randrange(20)
        return 1 if k == 0 else 60000 if k == 1 else 2 * rng.randrange(columns)

    body = struct.pack('>HHHH', width, left_at, right_at, array)
    body += struct.pack('>HH', first_left, left_count) + b''.join(struct.pack('>H', left()) for _ in range(left_count))
    body += struct.pack('>HH', first_right, right_count) + b''.join(struct.pack('>H', right()) for _ in range(right_count))
    body += b''.join(struct.pack('>h', rng.randrange(-120, 120)) for _ in range(rows * columns))
    return subtable(2, body)


def main():
    tool = sys.argv[1]
    source = sys.argv[2] if len(sys.argv) > 2 else '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf'
    fonts = plans = differing = 0
    with tempfile.TemporaryDirectory(prefix='vega-kern-') as directory:
        work = Path(directory)
        for seed in range(1, 5):
            for order in ('32', '23', '3', '2', '22', '33'):
                rng = random.Random(seed * 100 + len(order))
                font = TTFont(source)
                del font['GPOS']
                glyphs = len(font.getGlyphOrder())
                cmap = font.getBestCmap()
                latin = sorted({font.getGlyphID(cmap[c]) for c in range(0x20, 0x7F) if c in cmap})
                tables = [format3(rng, glyphs) if c == '3' else format2(rng, latin) for c in order]
                font['kern'] = newTable('kern')
                font['kern'].decompile(struct.pack('>HH', 0, len(tables)) + b''.join(tables), font)
                path = work / f'kern-{seed}-{order}.ttf'
                font.save(path)
                built = subprocess.run([tool, 'build', '--font', str(path), '--codes', '0x20-0x7e', '--shape',
                                        'Latn,Zyyy', '--runs', '3000', '-o', str(work / 'kern.vsym')],
                                       capture_output=True, text=True)
                results = [json.loads(line) for line in built.stdout.splitlines() if line.startswith('{"script"')]
                fonts += 1
                plans += len(results)
                if built.returncode or not results or any(r['differences'] for r in results):
                    differing += 1
                    print(f'seed {seed}, subtables {order}:\n{built.stderr[-2000:]}', file=sys.stderr)
    print(json.dumps({'fonts': fonts, 'plans': plans, 'differing_fonts': differing}))
    return 1 if differing else 0


if __name__ == '__main__':
    sys.exit(main())
