#!/usr/bin/env python3
"""The tables HarfBuzz's syllabic shapers read, as C (text/syllabic-data.c).

    python3 text/syllabic.py HARFBUZZ_SRC > text/syllabic-data.c
    python3 text/syllabic.py HARFBUZZ_SRC --check

HARFBUZZ_SRC is HarfBuzz 10.2.0's src/ directory; what is read from it:

- Each character's Indic category and position: hb-ot-shaper-indic-
  table.cc, the table HarfBuzz's gen-indic-table.py made from Unicode 16's
  IndicSyllabicCategory and IndicPositionalCategory with its own overrides,
  read cell for cell (Unicode here is 15.1: the table is taken as HarfBuzz
  has it). Khmer and Myanmar read the same table.

- Each character's category for the Universal Shaping Engine: hb-ot-
  shaper-use-table.hh, whose arrays and lookup (gen-use-table.py's) are
  carried over as they are, the categories named in them resolved.

- The syllable machines: hb-ot-shaper-*-machine.rl, the Ragel grammars.
  This compiles each to a DFA with Ragel's scanner semantics (the longest
  match from each position, ties to the pattern listed first), its symbols
  folded into classes that behave alike. --check runs the DFAs beside the
  machines Ragel generated from the same grammars (hb-ot-shaper-*-
  machine.hh) on random runs of symbols, and counts where they differ.

The output is deterministic: the same inputs give the same file.
"""
import random
import re
import sys
from pathlib import Path

HB = Path(sys.argv[1] if len(sys.argv) > 1 else '.')
MACHINES = ['indic', 'khmer', 'myanmar', 'use']
ALPHABET = 64  # categories past 63 are folded into 63, which only `any` takes


# ---- Grammars: Ragel's regular expressions over symbols

class Parser:
    """Ragel's expressions as the machines use them: names, `a | b`,
    `a . b` or `a b`, postfix `? * +`, parentheses."""

    def __init__(self, text, names):
        self.tokens = re.findall(r'[A-Za-z_][A-Za-z_0-9]*|[()|.?*+]', text)
        self.at = 0
        self.names = names

    def peek(self):
        return self.tokens[self.at] if self.at < len(self.tokens) else None

    def take(self):
        self.at += 1
        return self.tokens[self.at - 1]

    def alternation(self):
        node = self.concatenation()
        while self.peek() == '|':
            self.take()
            node = ('alt', node, self.concatenation())
        return node

    def concatenation(self):
        node = self.postfix()
        while self.peek() not in (None, '|', ')'):
            if self.peek() == '.':
                self.take()
            node = ('cat', node, self.postfix())
        return node

    def postfix(self):
        node = self.atom()
        while self.peek() in ('?', '*', '+'):
            node = ({'?': 'opt', '*': 'star', '+': 'plus'}[self.take()], node)
        return node

    def atom(self):
        token = self.take()
        if token == '(':
            node = self.alternation()
            assert self.take() == ')'
            return node
        return self.names[token]


def parse(text, names):
    parser = Parser(text, names)
    node = parser.alternation()
    assert parser.peek() is None, (text, parser.peek())
    return node


class Grammar:
    """A machine's .rl: its symbols (exports), syllable types (the enum), and
    the scanner's patterns in order, each with the type it finds."""

    def __init__(self, name):
        text = (HB / f'hb-ot-shaper-{name}-machine.rl').read_text()
        enum = re.search(r'enum \w+_syllable_type_t \{(.*?)\};', text, re.S).group(1)
        self.types = [t.strip() for t in enum.split(',') if t.strip()]
        body = '\n'.join(block for block in re.findall(r'%%\{(.*?)\}%%', text, re.S) if 'main :=' in block)
        body = re.sub(r'#[^\n]*', '', body)
        main = re.search(r'main := \|\*(.*?)\*\|;', body, re.S).group(1)
        rest = body.replace(re.search(r'main := \|\*.*?\*\|;', body, re.S).group(0), '')
        self.symbols = {name: int(value) for name, value in re.findall(r'export (\w+)\s*=\s*(\d+);', rest)}
        names = {name: ('sym', frozenset([value])) for name, value in self.symbols.items()}
        names['any'] = ('any',)
        for statement in rest.split(';'):
            statement = statement.strip()
            if not statement or statement.startswith('export'):
                continue
            name, expression = statement.split('=', 1)
            names[name.strip()] = parse(expression, names)
        self.patterns = []
        for pattern, action in re.findall(r'([^;{}]+?)=>\s*\{(.*?)\};', main, re.S):
            kind = re.search(r'found_syllable \((\w+)\)', action).group(1)
            self.patterns.append((parse(pattern, names), self.types.index(kind)))
        self.broken = next(i for i, t in enumerate(self.types) if t.endswith('broken_cluster'))


class NFA:
    def __init__(self):
        self.edges = []   # state: [(symbols or None for epsilon, target)]
        self.accept = {}  # state: pattern index

    def state(self):
        self.edges.append([])
        return len(self.edges) - 1

    def build(self, node):
        """Thompson's construction: (start, end) of node."""
        kind = node[0]
        if kind in ('sym', 'any'):
            a, b = self.state(), self.state()
            symbols = frozenset(range(ALPHABET)) if kind == 'any' else frozenset(min(s, ALPHABET - 1) for s in node[1])
            self.edges[a].append((symbols, b))
            return a, b
        if kind == 'alt':
            a, b = self.state(), self.state()
            for part in node[1:]:
                s, e = self.build(part)
                self.edges[a].append((None, s))
                self.edges[e].append((None, b))
            return a, b
        if kind == 'cat':
            s1, e1 = self.build(node[1])
            s2, e2 = self.build(node[2])
            self.edges[e1].append((None, s2))
            return s1, e2
        s, e = self.build(node[1])
        a, b = self.state(), self.state()
        self.edges[a].append((None, s))
        self.edges[e].append((None, b))
        if kind in ('opt', 'star'):
            self.edges[a].append((None, b))
        if kind in ('star', 'plus'):
            self.edges[e].append((None, s))
        return a, b

    def closure(self, states):
        seen, stack = set(states), list(states)
        while stack:
            for symbols, target in self.edges[stack.pop()]:
                if symbols is None and target not in seen:
                    seen.add(target)
                    stack.append(target)
        return frozenset(seen)


def machine(grammar):
    """The scanner's DFA, minimal: each state's transitions by symbol (None
    for none) and the syllable type it accepts (the first pattern's), or
    None; the start is state 0."""
    nfa = NFA()
    start = nfa.state()
    for index, (tree, kind) in enumerate(grammar.patterns):
        s, e = nfa.build(tree)
        nfa.edges[start].append((None, s))
        nfa.accept[e] = index
    first = nfa.closure([start])
    states, order, transitions = {first: 0}, [first], []
    for current in order:
        row = []
        for symbol in range(ALPHABET):
            targets = [t for s in current for symbols, t in nfa.edges[s] if symbols is not None and symbol in symbols]
            if not targets:
                row.append(None)
                continue
            target = nfa.closure(targets)
            if target not in states:
                states[target] = len(order)
                order.append(target)
            row.append(states[target])
        transitions.append(row)
    accepts = []
    for current in order:
        matched = [nfa.accept[s] for s in current if s in nfa.accept]
        accepts.append(grammar.patterns[min(matched)][1] if matched else None)
    # Moore's partition refinement
    n = len(transitions)
    block = list(accepts)
    while True:
        signature = [(block[s],) + tuple(None if t is None else block[t] for t in transitions[s]) for s in range(n)]
        numbering = {}
        refined = [numbering.setdefault(signature[s], len(numbering)) for s in range(n)]
        if len(numbering) == len(set(block)):
            break
        block = refined
    new = {}
    for s in range(n):
        new.setdefault(block[s], len(new))
    table, accept = [None] * len(new), [None] * len(new)
    for s in range(n):
        table[new[block[s]]] = [None if t is None else new[block[t]] for t in transitions[s]]
        accept[new[block[s]]] = accepts[s]
    # every symbol starts a syllable: the scanner never stalls
    assert all(table[0][symbol] is not None and accept[table[0][symbol]] is not None for symbol in range(ALPHABET))
    return table, accept


def classes(table):
    """Symbols whose columns are the same, folded."""
    columns, of = {}, []
    for symbol in range(ALPHABET):
        of.append(columns.setdefault(tuple(row[symbol] for row in table), len(columns)))
    return of, len(columns)


def scan(table, accept, symbols):
    """Syllables of a run of symbols by a DFA: (start, end, type)."""
    found, p = [], 0
    while p < len(symbols):
        state, end, kind = 0, None, None
        for q in range(p, len(symbols)):
            state = table[state][min(symbols[q], ALPHABET - 1)]
            if state is None:
                break
            if accept[state] is not None:
                end, kind = q + 1, accept[state]
        found.append((p, end, kind))
        p = end
    return found


# ---- The Indic table

def indic_table(grammars):
    """Code point to (category, position) for the characters HarfBuzz's
    table has, and the default for the others."""
    text = (HB / 'hb-ot-shaper-indic-table.cc').read_text()
    positions = {name: int(value) for name, value in
                 re.findall(r'(POS_\w+) = (\d+)', (HB / 'hb-ot-shaper-indic.hh').read_text())}
    machines = {'I': grammars['indic'].symbols, 'K': grammars['khmer'].symbols, 'M': grammars['myanmar'].symbols}
    long_names = {name: machines[m][cat] for name, m, cat in re.findall(r'#define (OT_\w+) ([IKM])_Cat\((\w+)\)', text)}
    short = {name: long_names[target] for name, target in re.findall(r'#define (_OT_\w+)\s+(OT_\w+)', text)}
    short.update({name: positions[target] for name, target in re.findall(r'#define (_POS_\w+)\s+(POS_\w+)', text)})

    def cell(s, m):
        return short['_OT_' + s], short['_POS_' + m]

    cells = {}
    table = text[text.index('static const uint16_t indic_table[]'):text.index('}; /* Table items:')]
    for start, row in re.findall(r'/\* ([0-9A-F]{4,5}) \*/(.*)', table):
        for k, (s, m) in enumerate(re.findall(r'_\((\w+),(\w+)\)', row)):
            cells[int(start, 16) + k] = cell(s, m)
    lookup = text[text.index('hb_indic_get_categories (hb_codepoint_t u)'):]
    for code, s, m in re.findall(r'u == 0x([0-9A-F]+)u\)\) return _\((\w+),(\w+)\);', lookup):
        cells[int(code, 16)] = cell(s, m)
    default = cell(*re.search(r'return _\((\w+),(\w+)\);\s*\}', lookup).groups())
    return cells, default


def runs_of(cells):
    """Runs of covered code points, gaps under 16 kept inside."""
    runs = []
    for code in sorted(cells):
        if runs and code - runs[-1][1] <= 16:
            runs[-1][1] = code
        else:
            runs.append([code, code])
    return runs


def use_table(grammar):
    """HarfBuzz's USE lookup: its arrays (categories resolved to numbers)
    and its expression, as C."""
    text = (HB / 'hb-ot-shaper-use-table.hh').read_text()
    part = text[text.index('#ifndef HB_OPTIMIZE_SIZE'):text.index('#else')]
    arrays = []
    for kind, name, n, body in re.findall(r'static const (uint8_t|uint16_t)\s+(hb_use_\w+)\[(\d+)\] =\s*\{(.*?)\};',
                                          part, re.S):
        values = [int(t) if t.isdigit() else grammar.symbols[t] for t in re.findall(r'[A-Za-z_]\w*|\d+', body)]
        assert len(values) == int(n), name
        arrays.append((kind, name.replace('hb_use_', 'use_'), values))
    expression = re.search(r'return (u<.*?);\n', part).group(1).replace('hb_use_', 'use_')
    assert expression.endswith(':O')
    return arrays, expression[:-1] + str(grammar.symbols['O'])


# ---- Checking against Ragel's machines (hb-ot-shaper-*-machine.hh)

def ragel(name):
    """A scanner from the flat tables and actions Ragel generated: symbols
    to (start, end, type)."""
    text = (HB / f'hb-ot-shaper-{name}-machine.hh').read_text()
    grammar = Grammar(name)

    def array(field):
        body = re.search(rf'_{name}_syllable_machine_{field}\[\] = \{{(.*?)\}};', text, re.S).group(1)
        return [int(v) for v in re.findall(r'-?\d+', body)]

    keys, spans, offsets, indicies = array('trans_keys'), array('key_spans'), array('index_offsets'), array('indicies')
    targets, actions = array('trans_targs'), array('trans_actions')
    to_state, from_state, eof_trans = array('to_state_actions'), array('from_state_actions'), array('eof_trans')
    start = int(re.search(rf'{name}_syllable_machine_start = (\d+);', text).group(1))

    def cases(switch):
        """The action cases of the switch on an array: number to code."""
        at = text.index(f'switch ( _{name}_syllable_machine_{switch}[')
        depth, begin = 0, text.index('{', at)
        for k in range(begin, len(text)):
            depth += {'{': 1, '}': -1}.get(text[k], 0)
            if depth == 0:
                body = text[begin + 1:k]
                break
        out, depth, current, k = {}, 0, None, 0
        while k < len(body):
            m = re.compile(r'case (\d+):').match(body, k) if depth == 0 else None
            if m:
                current = int(m.group(1))
                out[current] = ''
                k = m.end()
                continue
            depth += {'{': 1, '}': -1}.get(body[k], 0)
            if current is not None:
                out[current] += body[k]
            k += 1
        return out

    trans_cases, to_cases, from_cases = cases('trans_actions'), cases('to_state_actions'), cases('from_state_actions')

    def run(code, st):
        # the `switch( act )` of a backtracking action: only the chosen case
        m = re.search(r'switch\( act \) \{(.*)\}', code, re.S)
        if m:
            chosen = re.search(rf'case {st["act"]}:(.*?)break;', m.group(1), re.S)
            code = code[:m.start()] + (chosen.group(1) if chosen else '')
        for statement in re.findall(r'ts = p;|ts = 0;|te = p\+1;|te = p;|p--;|p = \(\(te\)\)-1;|act = \d+;|'
                                    r'found_syllable \(\w+\)', code):
            if statement == 'ts = p;': st['ts'] = st['p']
            elif statement == 'ts = 0;': st['ts'] = 0
            elif statement == 'te = p+1;': st['te'] = st['p'] + 1
            elif statement == 'te = p;': st['te'] = st['p']
            elif statement == 'p--;': st['p'] -= 1
            elif statement == 'p = ((te))-1;': st['p'] = st['te'] - 1
            elif statement.startswith('act'): st['act'] = int(statement[6:-1])
            else:
                kind = statement[len('found_syllable ('):-1]
                st['found'].append((st['ts'], st['te'], grammar.types.index(kind)))

    def scanner(symbols):
        st = {'ts': 0, 'te': 0, 'act': 0, 'p': 0, 'found': []}
        cs, pe = start, len(symbols)
        state = 'resume' if pe else 'test_eof'
        while True:
            if state == 'resume':
                if from_state[cs]:
                    run(from_cases[from_state[cs]], st)
                c, low, high, span = symbols[st['p']], keys[2 * cs], keys[2 * cs + 1], spans[cs]
                trans = indicies[offsets[cs] + (c - low if span > 0 and low <= c <= high else span)]
                state = 'eof_trans'
            if state == 'eof_trans':
                cs = targets[trans]
                if actions[trans]:
                    run(trans_cases[actions[trans]], st)
                if to_state[cs]:
                    run(to_cases[to_state[cs]], st)
                st['p'] += 1
                state = 'resume' if st['p'] != pe else 'test_eof'
                continue
            if state == 'test_eof':
                if st['p'] == pe and eof_trans[cs] > 0:
                    trans = eof_trans[cs] - 1
                    state = 'eof_trans'
                    continue
                return st['found']
    return scanner


def check(runs=100000):
    rng = random.Random(1)
    for name in MACHINES:
        grammar = Grammar(name)
        table, accept = machine(grammar)
        theirs = ragel(name)
        symbols = sorted(set(grammar.symbols.values())) + [0, 19, 63]
        differ = 0
        for _ in range(runs):
            run = [rng.choice(symbols) for _ in range(rng.randint(1, 14))]
            a, b = scan(table, accept, run), theirs(run)
            if a != b:
                differ += 1
                if differ <= 3:
                    print(name, 'differ:', run, a, b, file=sys.stderr)
        print(f'{name}: {len(table)} states, {runs} runs, {differ} differ')


# ---- The C file

def emit_machine(out, name, grammar):
    table, accept = machine(grammar)
    of, count = classes(table)
    dead = len(table)
    column_of = {}
    for symbol in range(ALPHABET):
        column_of.setdefault(of[symbol], symbol)
    width = 'uint8_t' if dead < 256 else 'uint16_t'
    out.append(f'/* {name} (hb-ot-shaper-{name}-machine.rl): {len(table)} states, {count} classes of symbol;'
               f' types {", ".join(grammar.types)}. */')
    out.append(f'static const uint8_t {name}_class[{ALPHABET}] = {{')
    for k in range(0, ALPHABET, 32):
        out.append('    ' + ', '.join(str(c) for c in of[k:k + 32]) + ',')
    out.append('};')
    out.append(f'static const {width} {name}_next[{len(table)}][{count}] = {{')
    for row in table:
        out.append('    {' + ', '.join(str(dead if row[column_of[k]] is None else row[column_of[k]])
                                      for k in range(count)) + '},')
    out.append('};')
    out.append(f'static const uint8_t {name}_accept[{len(table)}] = {{')
    cells = [str(0 if a is None else a + 1) for a in accept]
    for k in range(0, len(cells), 32):
        out.append('    ' + ', '.join(cells[k:k + 32]) + ',')
    out.append('};')
    out.append(f'const VegaMachine vega_{name}_machine = {{{name}_class, {count}, {len(table)}, {name}_next[0], '
               f'sizeof({name}_next[0][0]), {name}_accept, {grammar.broken + 1}}};')
    out.append('')


def main():
    if len(sys.argv) > 2 and sys.argv[2] == '--check':
        check()
        return
    grammars = {name: Grammar(name) for name in MACHINES}
    cells, default = indic_table(grammars)
    flat, starts = [], []
    for a, b in runs_of(cells):
        starts.append((a, b, len(flat)))
        flat.extend(cells.get(code, default) for code in range(a, b + 1))
    out = ['/* Generated by text/syllabic.py from HarfBuzz 10.2.0\'s sources; do not edit. */',
           '#include "syllabic.h"', '',
           '/* Each character\'s Indic category and position (hb-ot-shaper-indic-table.cc, Unicode 16):',
           ' * category | position << 8. */',
           f'static const uint16_t indic_cells[{len(flat)}] = {{']
    for k in range(0, len(flat), 12):
        out.append('    ' + ', '.join(f'0x{c | p << 8:04X}' for c, p in flat[k:k + 12]) + ',')
    out.append('};')
    out.append(f'static const struct {{ uint32_t first, last, at; }} indic_runs[{len(starts)}] = {{')
    for a, b, at in starts:
        out.append(f'    {{0x{a:04X}, 0x{b:04X}, {at}}},')
    out.append('};')
    out.append('')
    out.append('uint16_t vega_indic_categories(uint32_t u) {')
    out.append(f'    for (unsigned i = 0; i < {len(starts)} && u >= indic_runs[i].first; ++i)')
    out.append('        if (u <= indic_runs[i].last) return indic_cells[indic_runs[i].at + u - indic_runs[i].first];')
    out.append(f'    return 0x{default[0] | default[1] << 8:04X};')
    out.append('}')
    out.append('')
    arrays, expression = use_table(grammars['use'])
    out.append('/* Each character\'s USE category (hb-ot-shaper-use-table.hh, Unicode 16): HarfBuzz\'s arrays and')
    out.append(' * lookup. */')
    for kind, name, values in arrays:
        out.append(f'static const {kind} {name}[{len(values)}] = {{')
        for k in range(0, len(values), 16):
            out.append('    ' + ', '.join(str(v) for v in values[k:k + 16]) + ',')
        out.append('};')
    out.append('static unsigned use_b4(const uint8_t *a, unsigned i) { return (a[i >> 1] >> ((i & 1u) << 2)) & 15u; }')
    out.append(f'uint8_t vega_use_category(uint32_t u) {{ return (uint8_t)({expression}); }}')
    out.append('')
    out.append('/* The categories syllabic.h names are the machines\' own. */')
    for name, prefix in (('indic', 'VEGA_OT_'), ('khmer', 'VEGA_OT_'), ('myanmar', 'VEGA_OT_'), ('use', 'VEGA_USE_')):
        for symbol, value in sorted(grammars[name].symbols.items()):
            if name == 'myanmar' and symbol in ('IV', 'DB', 'GB'):
                continue  # Myanmar's names for V, N and PLACEHOLDER
            out.append(f'_Static_assert({prefix}{symbol.upper()} == {value}, "{name} {symbol}");')
    out.append('')
    out.append('/* The syllable machines: a DFA over classes of symbol; a state\'s accept is')
    out.append(' * the syllable type it ends, plus one. */')
    for name in MACHINES:
        emit_machine(out, name, grammars[name])
    sys.stdout.write('\n'.join(out) + '\n')


if __name__ == '__main__':
    main()
