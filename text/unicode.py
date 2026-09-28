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
    return ccc, decompositions, compositions, single


JOINING = ['U', 'D', 'R', 'L', 'C', 'T']
LINE = ['BK', 'CR', 'LF', 'NL', 'SP', 'ZW', 'ZWJ', 'CM', 'WJ', 'GL', 'BA', 'BB', 'B2', 'HY', 'CB', 'CL', 'CP', 'EX', 'IN',
        'NS', 'OP', 'QU', 'IS', 'NU', 'PO', 'PR', 'SY', 'AI', 'AK', 'AL', 'AP', 'AS', 'CJ', 'EB', 'EM', 'H2', 'H3', 'HL',
        'ID', 'JL', 'JV', 'JT', 'RI', 'SA', 'SG', 'VF', 'VI', 'XX']
WIDTHS = ['A', 'F', 'H', 'N', 'Na', 'W']
GRAPHEME = ['Other', 'CR', 'LF', 'Control', 'Extend', 'ZWJ', 'Regional_Indicator', 'Prepend', 'SpacingMark', 'L', 'V', 'T',
            'LV', 'LVT']
CONJUNCT = ['None', 'Linker', 'Consonant', 'Extend']
WORD = ['Other', 'CR', 'LF', 'Newline', 'Extend', 'ZWJ', 'Regional_Indicator', 'Format', 'Katakana', 'Hebrew_Letter',
        'ALetter', 'Single_Quote', 'Double_Quote', 'MidNumLet', 'MidLetter', 'MidNum', 'Numeric', 'ExtendNumLet',
        'WSegSpace']
SENTENCE = ['Other', 'CR', 'LF', 'Extend', 'Sep', 'Format', 'Sp', 'Lower', 'Upper', 'OLetter', 'Numeric', 'ATerm',
            'SContinue', 'STerm', 'Close']
CATEGORIES = ['Cn', 'Lu', 'Ll', 'Lt', 'Lm', 'Lo', 'Mn', 'Mc', 'Me', 'Nd', 'Nl', 'No', 'Pc', 'Pd', 'Ps', 'Pe', 'Pi', 'Pf',
              'Po', 'Sm', 'Sc', 'Sk', 'So', 'Zs', 'Zl', 'Zp', 'Cc', 'Cf', 'Cs', 'Co']


def scripts():
    """Each code point's Script (Unknown by default), and the scripts' names in order."""
    names = ['Unknown', 'Common', 'Inherited']
    values = [0] * (LAST + 1)
    for first, last, fields, missing in ranges('Scripts.txt'):
        if missing:
            continue
        if fields[0] not in names:
            names.append(fields[0])
        values[first:last + 1] = [names.index(fields[0])] * (last - first + 1)
    return values, names


def aliases(prop):
    """A property's long value names to its short ones (PropertyValueAliases.txt)."""
    out = {}
    for line in (UCD / 'PropertyValueAliases.txt').read_text(encoding='utf-8').splitlines():
        fields = [f.strip() for f in line.split('#', 1)[0].split(';')]
        if len(fields) >= 3 and fields[0] == prop:
            for name in fields[2:]:
                out[name] = fields[1]
    return out


def script_codes(names):
    """Each script's ISO 15924 code (Latn for Latin), in the order of names."""
    codes = {}
    for line in (UCD / 'PropertyValueAliases.txt').read_text(encoding='utf-8').splitlines():
        fields = [f.strip() for f in line.split('#', 1)[0].split(';')]
        if len(fields) >= 3 and fields[0] == 'sc':
            codes[fields[2]] = fields[1]
    return [codes[name] for name in names]


def main():
    bidi = derived('extracted/DerivedBidiClass.txt', BIDI, LONG)
    joining = derived('extracted/DerivedJoiningType.txt', JOINING, {'Non_Joining': 'U'})
    category = derived('extracted/DerivedGeneralCategory.txt', CATEGORIES)
    line = derived('extracted/DerivedLineBreak.txt', LINE, aliases('lb'))
    width = derived('extracted/DerivedEastAsianWidth.txt', WIDTHS, aliases('ea'))
    brackets_ea = [c for c in range(LAST + 1) if LINE[line[c]] in ('OP', 'CP') and WIDTHS[width[c]] in ('F', 'W', 'H')]
    grapheme = derived('auxiliary/GraphemeBreakProperty.txt', GRAPHEME)
    conjunct = [0] * (LAST + 1)
    for first, last, fields, missing in ranges('DerivedCoreProperties.txt'):
        if not missing and fields[0] == 'InCB':
            conjunct[first:last + 1] = [CONJUNCT.index(fields[1])] * (last - first + 1)
    grapheme = [g | c << 4 for g, c in zip(grapheme, conjunct)]
    word = derived('auxiliary/WordBreakProperty.txt', WORD)
    sentence = derived('auxiliary/SentenceBreakProperty.txt', SENTENCE)
    script, script_names = scripts()
    for index, name in ((0, 'Unknown'), (1, 'Common'), (2, 'Inherited'), (3, 'Latin'), (4, 'Greek'), (5, 'Cyrillic'),
                        (7, 'Hebrew'), (8, 'Arabic'), (26, 'Hangul')):
        assert script_names[index] == name, f'text.h names {name} {index}: now {script_names.index(name)}'
    table, size = two_stage('bidi', bidi)
    ccc, decompositions, compositions, single = normalization()
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
    for name, values, kind, fallback in (('joining', joining, 'VegaJoining', 'VEGA_JOINING_U'),
                                         ('line_break', line, 'VegaLineBreak', 'VEGA_LB_XX'),
                                         ('grapheme', grapheme, 'uint8_t', '0'),
                                         ('word', word, 'uint8_t', '0'),
                                         ('sentence', sentence, 'uint8_t', '0'),
                                         ('category', category, 'VegaCategory', 'VEGA_CN'),
                                         ('script', script, 'VegaScript', 'VEGA_SCRIPT_UNKNOWN')):
        table, size = two_stage(name, values)
        print()
        print(f'/* {name} ({size} bytes). */')
        print(table)
        print()
        print(f'{kind} vega_{name}(uint32_t code) {{')
        print(f'    return code > 0x10FFFF ? {fallback} : ({kind}){name}_values[{name}_blocks[code >> 8] * 256u + (code & 255)];')
        print('}')
    print()
    print(f'static const char *const script_names[{len(script_names)}] = {{')
    for i in range(0, len(script_names), 6):
        print('    ' + ', '.join(f'"{n}"' for n in script_names[i:i + 6]) + ',')
    print('};')
    print()
    print('const char *vega_script_name(VegaScript script) {')
    print(f'    return (unsigned)script < {len(script_names)} ? script_names[script] : "Unknown";')
    print('}')
    codes = script_codes(script_names)
    print()
    print('/* Their ISO 15924 codes, as tags (the first letter in the high byte). */')
    print(f'static const uint32_t script_codes[{len(codes)}] = {{')
    for i in range(0, len(codes), 8):
        print('    ' + ', '.join('0x%08X' % int.from_bytes(c.encode(), 'big') for c in codes[i:i + 8]) + ',')
    print('};')
    print()
    print('uint32_t vega_script_code(VegaScript script) {')
    print(f'    return script_codes[(unsigned)script < {len(codes)} ? script : 0];')
    print('}')
    print()
    print('/* Opening and closing punctuation (OP, CP) of East_Asian_Width F, W or H. */')
    print(f'static const uint32_t east_asian_brackets[{len(brackets_ea)}] = {{')
    for i in range(0, len(brackets_ea), 10):
        print('    ' + ', '.join(f'0x{c:04X}' for c in brackets_ea[i:i + 10]) + ',')
    print('};')
    print('''
bool vega_east_asian_bracket(uint32_t code) {
    size_t low = 0, high = sizeof(east_asian_brackets) / sizeof(*east_asian_brackets);
    while (low < high) {
        size_t middle = (low + high) / 2;
        if (east_asian_brackets[middle] < code) low = middle + 1;
        else high = middle;
    }
    return low < sizeof(east_asian_brackets) / sizeof(*east_asian_brackets) && east_asian_brackets[low] == code;
}''')
    pictographic = [(first, last) for first, last, fields, missing in ranges('emoji/emoji-data.txt')
                    if not missing and fields[0] == 'Extended_Pictographic']
    print()
    print('/* Extended_Pictographic (emoji-data.txt), as sorted ranges. */')
    print(f'static const uint32_t pictographic[{len(pictographic)}][2] = {{')
    for i in range(0, len(pictographic), 6):
        print('    ' + ', '.join('{0x%04X, 0x%04X}' % r for r in pictographic[i:i + 6]) + ',')
    print('};')
    print('''
bool vega_extended_pictographic(uint32_t code) {
    size_t low = 0, high = sizeof(pictographic) / sizeof(*pictographic);
    while (low < high) {
        size_t middle = (low + high) / 2;
        if (pictographic[middle][1] < code) low = middle + 1;
        else high = middle;
    }
    return low < sizeof(pictographic) / sizeof(*pictographic) && pictographic[low][0] <= code;
}''')
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
    print('/* Canonical decomposition mappings, one step (UnicodeData.txt): code, first, second (0 for one). */')
    print(f'static const uint32_t steps[{len(single)}][3] = {{')
    for code in sorted(single):
        parts = single[code] + [0]
        print(f'    {{0x{code:04X}, 0x{parts[0]:04X}, 0x{parts[1]:04X}}},')
    print('};')
    print()
    print('bool vega_decomposition_step(uint32_t code, uint32_t *first, uint32_t *second) {')
    print('    long row = find(&steps[0][0], sizeof(steps) / sizeof(*steps), 3, code);')
    print('    if (row < 0) return false;')
    print('    *first = steps[row][1];')
    print('    *second = steps[row][2];')
    print('    return true;')
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
