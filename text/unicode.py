#!/usr/bin/env python3
"""The Unicode properties Vega's text needs, as C tables (text/unicode-data.c),
from the Unicode Character Database (15.1 on Debian: /usr/share/unicode).

    python3 text/unicode.py [UCD directory] > text/unicode-data.c

A property of every code point is a two-stage table: the code point's upper
bits pick a block of 256 values (blocks that repeat are kept once). Sparse
properties (paired brackets, mirrors) are sorted arrays, searched by code
point. The output is deterministic: the same UCD gives the same file.
"""
import re
import sys
from pathlib import Path

UCD = Path(sys.argv[1] if len(sys.argv) > 1 else '/usr/share/unicode')

BIDI = ['L', 'R', 'AL', 'EN', 'ES', 'ET', 'AN', 'CS', 'NSM', 'BN', 'B', 'S', 'WS', 'ON',
        'LRE', 'LRO', 'RLE', 'RLO', 'PDF', 'LRI', 'RLI', 'FSI', 'PDI']
LONG = {'Left_To_Right': 'L', 'Right_To_Left': 'R', 'Arabic_Letter': 'AL', 'European_Number': 'EN',
        'European_Separator': 'ES', 'European_Terminator': 'ET', 'Arabic_Number': 'AN', 'Common_Separator': 'CS',
        'Nonspacing_Mark': 'NSM', 'Boundary_Neutral': 'BN', 'Paragraph_Separator': 'B', 'Segment_Separator': 'S',
        'White_Space': 'WS', 'Other_Neutral': 'ON', 'Left_To_Right_Embedding': 'LRE',
        'Left_To_Right_Override': 'LRO', 'Right_To_Left_Embedding': 'RLE', 'Right_To_Left_Override': 'RLO',
        'Pop_Directional_Format': 'PDF', 'Left_To_Right_Isolate': 'LRI', 'Right_To_Left_Isolate': 'RLI',
        'First_Strong_Isolate': 'FSI', 'Pop_Directional_Isolate': 'PDI'}
LAST = 0x10FFFF


def ranges(path):
    """(first, last, value, missing) of each line of a UCD file, @missing lines included."""
    for line in (UCD / path).read_text(encoding='utf-8').splitlines():
        missing = line.startswith('# @missing:')
        body = line[len('# @missing:'):] if missing else line.split('#', 1)[0]
        if not body.strip():
            continue
        fields = [f.strip() for f in body.split(';')]
        span = fields[0].split('..')
        yield int(span[0], 16), int(span[-1], 16), fields[1:], missing


def derived(path, names, long_names=None):
    """A value for every code point: the defaults of the @missing lines, then the data."""
    values = [0] * (LAST + 1)
    lines = list(ranges(path))
    for first, last, fields, missing in lines:
        if missing:
            name = (long_names or {}).get(fields[0], fields[0])
            values[first:last + 1] = [names.index(name)] * (last - first + 1)
    for first, last, fields, missing in lines:
        if not missing:
            values[first:last + 1] = [names.index(fields[0])] * (last - first + 1)
    return values


def two_stage(name, values):
    """C for a two-stage table of u8 values, and the lookup's name."""
    blocks, index, stage1 = [], {}, []
    for start in range(0, LAST + 1, 256):
        block = tuple(values[start:start + 256])
        if block not in index:
            index[block] = len(blocks)
            blocks.append(block)
        stage1.append(index[block])
    out = [f'static const uint16_t {name}_blocks[{len(stage1)}] = {{']
    for i in range(0, len(stage1), 16):
        out.append('    ' + ', '.join(str(v) for v in stage1[i:i + 16]) + ',')
    out.append('};')
    out.append(f'static const uint8_t {name}_values[{len(blocks) * 256}] = {{')
    for block in blocks:
        for i in range(0, 256, 32):
            out.append('    ' + ','.join(str(v) for v in block[i:i + 32]) + ',')
    out.append('};')
    return '\n'.join(out), len(stage1) * 2 + len(blocks) * 256


def unicode_data():
    """Each code point's UnicodeData.txt fields (ranges expanded)."""
    rows, start = {}, None
    for line in (UCD / 'UnicodeData.txt').read_text(encoding='utf-8').splitlines():
        fields = line.split(';')
        code = int(fields[0], 16)
        if fields[1].endswith(', First>'):
            start = code
            continue
        if fields[1].endswith(', Last>'):
            for c in range(start, code + 1):
                rows[c] = fields
            continue
        rows[code] = fields
    return rows


def normalization():
    """Canonical combining classes, full canonical decompositions (Hangul
    syllables aside: they are algorithmic) and the primary composites."""
    rows = unicode_data()
    ccc = [0] * (LAST + 1)
    single = {}
    for code, fields in rows.items():
        ccc[code] = int(fields[3])
        if fields[5] and not fields[5].startswith('<'):
            single[code] = [int(x, 16) for x in fields[5].split()]

    def full(code):
        return [c for d in single[code] for c in (full(d) if d in single else [d])]

    excluded = set()
    for first, last, fields, missing in ranges('DerivedNormalizationProps.txt'):
        if not missing and fields[0] == 'Full_Composition_Exclusion':
            excluded.update(range(first, last + 1))
    decompositions = {code: full(code) for code in single}
    compositions = sorted((d[0], d[1], code) for code, d in single.items() if len(d) == 2 and code not in excluded)
    return ccc, decompositions, compositions


def main():
    bidi = derived('extracted/DerivedBidiClass.txt', BIDI, LONG)
    table, size = two_stage('bidi', bidi)
    ccc, decompositions, compositions = normalization()
    ccc_table, ccc_size = two_stage('ccc', ccc)
    brackets = []
    for first, _, fields, missing in ranges('BidiBrackets.txt'):
        if not missing:
            brackets.append((first, int(fields[0], 16), 1 if fields[1] == 'o' else 2))
    mirrors = [(first, int(fields[0], 16)) for first, _, fields, missing in ranges('BidiMirroring.txt') if not missing]
    version = re.search(r'(\d+\.\d+\.\d+)', (UCD / 'ReadMe.txt').read_text()) if (UCD / 'ReadMe.txt').exists() else None
    print('/* Generated by text/unicode.py from the Unicode Character Database'
          f'{" " + version.group(1) if version else ""}: do not edit. */')
    print('#include "text.h"')
    print()
    print(f'/* Bidi_Class ({size} bytes). */')
    print(table)
    print()
    print('VegaBidiClass vega_bidi_class(uint32_t code) {')
    print('    return code > 0x10FFFF ? VEGA_BIDI_L : (VegaBidiClass)bidi_values[bidi_blocks[code >> 8] * 256u + (code & 255)];')
    print('}')
    print()
    print(f'/* Bidi_Paired_Bracket and Bidi_Paired_Bracket_Type (1 open, 2 close), by code point. */')
    print(f'static const uint32_t brackets[{len(brackets)}][3] = {{')
    for code, pair, kind in sorted(brackets):
        print(f'    {{0x{code:04X}, 0x{pair:04X}, {kind}}},')
    print('};')
    print()
    print(f'/* Bidi_Mirroring_Glyph, by code point. */')
    print(f'static const uint32_t mirrors[{len(mirrors)}][2] = {{')
    for code, mirror in sorted(mirrors):
        print(f'    {{0x{code:04X}, 0x{mirror:04X}}},')
    print('};')
    print('''
/* The row of a sorted table of rows whose first value is code, or -1. */
static long find(const uint32_t *rows, size_t count, size_t width, uint32_t code) {
    size_t low = 0, high = count;
    while (low < high) {
        size_t middle = (low + high) / 2;
        if (rows[middle * width] < code) low = middle + 1;
        else high = middle;
    }
    return low < count && rows[low * width] == code ? (long)low : -1;
}

int vega_bracket(uint32_t code, uint32_t *pair) {
    long row = find(&brackets[0][0], sizeof(brackets) / sizeof(*brackets), 3, code);
    if (row < 0) return 0;
    *pair = brackets[row][1];
    return (int)brackets[row][2];
}

uint32_t vega_mirror(uint32_t code) {
    long row = find(&mirrors[0][0], sizeof(mirrors) / sizeof(*mirrors), 2, code);
    return row < 0 ? 0 : mirrors[row][1];
}''')
    print()
    print(f'/* Canonical_Combining_Class ({ccc_size} bytes). */')
    print(ccc_table)
    print()
    print('uint8_t vega_combining_class(uint32_t code) {')
    print('    return code > 0x10FFFF ? 0 : ccc_values[ccc_blocks[code >> 8] * 256u + (code & 255)];')
    print('}')
    pool, rows = [], []
    for code in sorted(decompositions):
        rows.append((code, len(pool), len(decompositions[code])))
        pool.extend(decompositions[code])
    print()
    print('/* Full canonical decompositions (Hangul syllables aside): code, start in the pool, length. */')
    print(f'static const uint32_t decompositions[{len(rows)}][3] = {{')
    for code, start, length in rows:
        print(f'    {{0x{code:04X}, {start}, {length}}},')
    print('};')
    print(f'static const uint32_t decomposed[{len(pool)}] = {{')
    for i in range(0, len(pool), 12):
        print('    ' + ', '.join(f'0x{c:04X}' for c in pool[i:i + 12]) + ',')
    print('};')
    print()
    print('size_t vega_decomposition(uint32_t code, const uint32_t **out) {')
    print('    long row = find(&decompositions[0][0], sizeof(decompositions) / sizeof(*decompositions), 3, code);')
    print('    if (row < 0) return 0;')
    print('    *out = decomposed + decompositions[row][1];')
    print('    return decompositions[row][2];')
    print('}')
    print()
    print('/* The primary composites: first, second, composite, by first then second. */')
    print(f'static const uint32_t compositions[{len(compositions)}][3] = {{')
    for first, second, code in compositions:
        print(f'    {{0x{first:04X}, 0x{second:04X}, 0x{code:04X}}},')
    print('};')
    print('''
uint32_t vega_composition(uint32_t first, uint32_t second) {
    size_t low = 0, high = sizeof(compositions) / sizeof(*compositions);
    while (low < high) {
        size_t middle = (low + high) / 2;
        const uint32_t *row = compositions[middle];
        if (row[0] < first || (row[0] == first && row[1] < second)) low = middle + 1;
        else high = middle;
    }
    return low < sizeof(compositions) / sizeof(*compositions) && compositions[low][0] == first &&
           compositions[low][1] == second ? compositions[low][2] : 0;
}''')


if __name__ == '__main__':
    main()
