"""Generate the syllable scanners of the HarfBuzz-model shapers as minimal DFAs.

The grammars are HarfBuzz's syllable machines: hb-ot-shaper-indic-machine.rl (the Microsoft Indic shaping
specifications), hb-ot-shaper-khmer-machine.rl, hb-ot-shaper-myanmar-machine.rl and hb-ot-shaper-use-machine.rl
(the Universal Shaping Engine). Each scanner takes the longest match at each position; on equal length the
earlier pattern wins. A table is minimized over the outcomes its shaper distinguishes.

    src/text/scripts/indic_machine.inc    indic_machine_next[state][IndicCategory], indic_machine_outcome
    src/text/scripts/khmer_machine.inc    khmer_next[state * khmer_stride + khmer_column[category]], khmer_outcome
    src/text/scripts/myanmar_machine.inc  as Khmer
    src/text/scripts/use_machine.inc      as Khmer

The Khmer, Myanmar and USE tables merge categories whose columns are equal: column[category] selects the
column. State 0 is the start and 0 as a target means no transition. Their rows overlap: a state's row starts at
state * stride, and rows share the cells where they hold the same target (mostly no transition), so they index
like plain [state][column] arrays.

No inputs; run from the repository root: python tools/syllable_machines.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1] / 'src' / 'text' / 'scripts'


class Grammar:
    """Regular-expression builders over one category list."""

    def __init__(self, categories):
        self.categories = categories
        self.index = {name: i for i, name in enumerate(categories)}

    def sym(self, *names):
        return ('sym', frozenset(self.index[n] for n in names))

    def any(self):
        return ('sym', frozenset(range(len(self.categories))))


def cat(*parts):
    node = parts[0]
    for part in parts[1:]:
        node = ('cat', node, part)
    return node


def alt(*parts):
    node = parts[0]
    for part in parts[1:]:
        node = ('alt', node, part)
    return node


def star(node):
    return ('star', node)


def opt(node):
    return ('opt', node)


def plus(node):
    return cat(node, star(node))


class Nfa:
    def __init__(self):
        self.epsilon, self.edges = [], []

    def state(self):
        self.epsilon.append([])
        self.edges.append([])
        return len(self.epsilon) - 1

    def build(self, node):
        kind = node[0]
        if kind == 'sym':
            first, last = self.state(), self.state()
            self.edges[first].append((node[1], last))
            return first, last
        if kind == 'cat':
            a, b = self.build(node[1])
            c_, d = self.build(node[2])
            self.epsilon[b].append(c_)
            return a, d
        first, last = self.state(), self.state()
        if kind == 'alt':
            for part in node[1:]:
                a, b = self.build(part)
                self.epsilon[first].append(a)
                self.epsilon[b].append(last)
            return first, last
        a, b = self.build(node[1])
        self.epsilon[first] += [a, last]
        self.epsilon[b].append(last)
        if kind == 'star':
            self.epsilon[b].append(a)
        return first, last


def dfa(categories, patterns):
    """patterns: [(node, outcome)] in priority order; returns the minimal table and outcomes."""
    nfa = Nfa()
    start = nfa.state()
    accepting = {}
    for priority, (pattern, _) in enumerate(patterns):
        first, last = nfa.build(pattern)
        nfa.epsilon[start].append(first)
        accepting[last] = priority

    def closure(states):
        stack, seen = list(states), set(states)
        while stack:
            for target in nfa.epsilon[stack.pop()]:
                if target not in seen:
                    seen.add(target)
                    stack.append(target)
        return frozenset(seen)

    states = [closure([start])]
    index = {states[0]: 0}
    table, labels = [], []
    while len(table) < len(states):
        current = states[len(table)]
        row = []
        for symbol in range(len(categories)):
            following = closure([t for s in current for (symbols, t) in nfa.edges[s] if symbol in symbols])
            if not following:
                row.append(-1)
                continue
            if following not in index:
                index[following] = len(states)
                states.append(following)
            row.append(index[following])
        table.append(row)
        matched = [accepting[s] for s in current if s in accepting]
        labels.append(patterns[min(matched)][1] if matched else 0)
    labels[0] = 0  # The scanner never takes an empty match.
    return minimize(table, labels)


def minimize(table, labels):
    blocks = {}
    for state, label in enumerate(labels):
        blocks.setdefault(label, []).append(state)
    partition = list(blocks.values())
    while True:
        owner = {s: b for b, block in enumerate(partition) for s in block}
        split = {}
        for state in range(len(table)):
            split.setdefault((owner[state], tuple(owner[t] if t >= 0 else -1 for t in table[state])), []).append(state)
        if len(split) == len(partition):
            break
        partition = list(split.values())
    # The start state is 0 and nothing returns to it, so 0 can mean "no transition".
    partition.sort(key=lambda block: (0 not in block, min(block)))
    owner = {s: b for b, block in enumerate(partition) for s in block}
    assert all(owner[t] != 0 for row in table for t in row if t >= 0)
    minimal = [[owner[t] if t >= 0 else 0 for t in table[block[0]]] for block in partition]
    return minimal, [labels[block[0]] for block in partition]


def rows(values, per=40):
    return ['    ' + ','.join(str(v) for v in values[i:i + per]) + ',' for i in range(0, len(values), per)]


def overlay(table, labels, orders):
    """Lays the rows over each other at the stride and column order that take the fewest bytes: a state is renumbered
    to the slot its row starts at (slot * stride), and a row only goes where every cell it shares with the rows already
    placed holds the same target. Slots without a state are never reached. Returns the flat table, the outcomes by
    slot, the stride and the column order."""
    best = None
    for order in orders:
        cells = [[row[c] for c in order] for row in table]
        width = len(order)
        for stride in range(1, width + 1):
            flat, slot_of, slot = [], {}, 0
            while len(slot_of) < len(cells) and slot < 256:
                base = slot * stride
                # The start stays state 0; after it, the fullest row that fits.
                fits = [s for s in (range(len(cells)) if slot else [0]) if s not in slot_of and all(
                    base + c >= len(flat) or flat[base + c] is None or flat[base + c] == v for c, v in enumerate(cells[s]))]
                if fits:
                    state = max(fits, key=lambda s: sum(1 for v in cells[s] if v))
                    slot_of[state] = slot
                    flat[len(flat):] = [None] * (base + width - len(flat))
                    flat[base:base + width] = cells[state]
                slot += 1
            if len(slot_of) < len(cells) or (best and len(flat) + slot >= best[0]):
                continue
            outcomes = [0] * slot
            for state, at in slot_of.items():
                outcomes[at] = labels[state]
            best = (len(flat) + slot, [slot_of[v] if v else 0 for v in flat], outcomes, stride, order)
    return best[1:]


# Indic ----------------------------------------------------------------------------------------------------------------

def indic():
    # IndicCategory order in src/text/scripts/indic_syllables.hpp (HarfBuzz numbering; SMPst follows CS).
    g = Grammar(['X', 'C', 'V', 'N', 'H', 'ZWNJ', 'ZWJ', 'M', 'SM', 'A', 'PLACEHOLDER', 'DOTTEDCIRCLE', 'RS', 'MPst',
                 'Repha', 'Ra', 'CM', 'Symbol', 'CS', 'SMPst'])
    s = g.sym
    c = s('C', 'Ra')
    n = cat(opt(cat(opt(s('ZWNJ')), s('RS'))), opt(cat(s('N'), opt(s('N')))))
    z = s('ZWJ', 'ZWNJ')
    reph = alt(cat(s('Ra'), s('H')), s('Repha'))
    sm = s('SM', 'SMPst')
    cn = cat(c, opt(s('ZWJ')), opt(n))
    symbol = cat(s('Symbol'), opt(s('N')))
    matra_group = cat(star(z), alt(s('M'), cat(opt(sm), s('MPst'))), opt(s('N')), opt(s('H')))
    syllable_tail = cat(opt(cat(opt(z), sm, opt(sm), opt(s('ZWNJ')))), star(s('A')))
    halant_group = cat(opt(z), s('H'), opt(cat(s('ZWJ'), opt(s('N')))))
    final_halant_group = alt(halant_group, cat(s('H'), s('ZWNJ')))
    medial_group = opt(s('CM'))
    halant_or_matra_group = alt(final_halant_group, star(matra_group))
    complex_syllable_tail = cat(star(cat(halant_group, cn)), medial_group, halant_or_matra_group, syllable_tail)
    consonant_syllable = cat(opt(s('Repha', 'CS')), cn, complex_syllable_tail)
    vowel_syllable = cat(opt(reph), s('V'), opt(n), alt(s('ZWJ'), complex_syllable_tail))
    standalone_cluster = cat(alt(cat(opt(s('Repha', 'CS')), s('PLACEHOLDER')), cat(opt(reph), s('DOTTEDCIRCLE'))), opt(n),
                             complex_syllable_tail)
    symbol_cluster = cat(symbol, syllable_tail)
    broken_cluster = cat(opt(reph), opt(n), complex_syllable_tail)
    # Outcome: 1 Indic syllable, 2 broken cluster, 3 plain cluster.
    patterns = [(consonant_syllable, 1), (vowel_syllable, 1), (standalone_cluster, 1), (symbol_cluster, 3),
                (s('SMPst'), 3), (broken_cluster, 2), (g.any(), 3)]
    table, labels = dfa(g.categories, patterns)
    assert len(table) < 256
    lines = ['// Generated by tools/syllable_machines.py; do not edit.',
             '// HarfBuzz hb-ot-shaper-indic-machine.rl grammar as a minimal longest-match DFA. Rows are states (0 is the',
             f'// start, and 0 as a target means no transition), columns IndicCategory values. {len(table)} states.',
             f'constexpr u8 indic_machine_next[{len(table)}][{len(g.categories)}]{{']
    lines += ['    {' + ','.join(str(v) for v in row) + '},' for row in table]
    lines.append('};')
    lines.append('// Outcome of the longest match ending in each state: 0 none, 1 Indic syllable, 2 broken, 3 plain.')
    lines.append(f'constexpr u8 indic_machine_outcome[{len(labels)}]{{')
    lines += rows(labels)
    lines.append('};')
    return 'indic_machine.inc', lines


# Khmer, Myanmar, USE: merged columns ----------------------------------------------------------------------------------

def compressed(name, source, categories, patterns, outcomes):
    """A table with equal columns merged: name_column[category], name_next[state * name_stride + column], name_outcome."""
    table, labels = dfa(categories, patterns)
    columns, column_of = [], []
    for c in range(len(categories)):
        column = tuple(row[c] for row in table)
        if column not in columns:
            columns.append(column)
        column_of.append(columns.index(column))
    merged = [[column[state] for column in columns] for state in range(len(table))]
    # Sparse columns at both ends and dense ones in the middle let neighbouring rows overlap the most.
    dense = sorted(range(len(columns)), key=lambda c: sum(1 for v in columns[c] if v))
    flat, labels, stride, order = overlay(merged, labels, [list(range(len(columns))), dense[1::2] + dense[0::2][::-1]])
    column_of = [order.index(c) for c in column_of]
    lines = ['// Generated by tools/syllable_machines.py; do not edit.',
             f'// HarfBuzz {source} grammar as a minimal longest-match DFA: {len(table)} states, {len(columns)} columns.',
             f'// {name}_column maps a category (the enumerator order below) to its column; state s reads its row at',
             f'// s * {name}_stride of {name}_next, and rows overlap where they agree (0 is the start, and 0 as a target means',
             '// no transition).',
             '// Categories: ' + ' '.join(categories) + '.',
             f'constexpr u8 {name}_column[{len(categories)}]{{']
    lines += rows(column_of)
    lines.append('};')
    lines.append(f'constexpr u8 {name}_stride={stride};')
    lines.append(f'constexpr u8 {name}_next[{len(flat)}]{{')
    lines += rows(flat)
    lines.append('};')
    lines.append(f'// Outcome of the longest match ending in each state: 0 none, else {outcomes}.')
    lines.append(f'constexpr u8 {name}_outcome[{len(labels)}]{{')
    lines += rows(labels)
    lines.append('};')
    return f'{name}_machine.inc', lines


def khmer():
    # K order in src/text/scripts/syllabic_shape.cpp.
    g = Grammar(['X', 'C', 'V', 'H', 'ZWNJ', 'ZWJ', 'PLACEHOLDER', 'DOTTEDCIRCLE', 'Ra', 'VAbv', 'VBlw', 'VPre', 'VPst',
                 'Robatic', 'Xgroup', 'Ygroup'])
    s = g.sym
    c = s('C', 'Ra', 'V')
    cn = cat(c, opt(cat(opt(s('ZWJ', 'ZWNJ')), s('Robatic'))))
    joiner = s('ZWJ', 'ZWNJ')
    xgroup = star(cat(star(joiner), s('Xgroup')))
    ygroup = star(s('Ygroup'))
    matra_group = cat(opt(s('VPre')), xgroup, opt(s('VBlw')), xgroup, opt(cat(opt(joiner), s('VAbv'))), xgroup, opt(s('VPst')))
    syllable_tail = cat(xgroup, matra_group, xgroup, opt(cat(s('H'), c)), ygroup)
    broken_cluster = cat(opt(s('Robatic')), star(cat(s('H'), cn)), alt(s('H'), syllable_tail))
    consonant_syllable = cat(alt(cn, s('PLACEHOLDER'), s('DOTTEDCIRCLE')), broken_cluster)
    patterns = [(consonant_syllable, 1), (broken_cluster, 2), (g.any(), 3)]
    return compressed('khmer', 'hb-ot-shaper-khmer-machine.rl', g.categories, patterns,
                      '1 consonant syllable, 2 broken cluster, 3 non-Khmer cluster')


def myanmar():
    # M order in src/text/scripts/syllabic_shape.cpp.
    g = Grammar(['X', 'C', 'IV', 'DB', 'H', 'ZWNJ', 'ZWJ', 'SM', 'A', 'GB', 'DOTTEDCIRCLE', 'Ra', 'CS', 'SMPst', 'VAbv',
                 'VBlw', 'VPre', 'VPst', 'As', 'MH', 'MR', 'MW', 'MY', 'PT', 'VS', 'ML'])
    s = g.sym
    j = s('ZWJ', 'ZWNJ')
    k = cat(s('Ra'), s('As'), s('H'))
    sm = s('SM', 'SMPst')
    c = s('C', 'Ra')
    medial_group = cat(opt(s('MY')), opt(s('As')), opt(s('MR')),
                       opt(cat(alt(cat(s('MW'), opt(s('MH')), opt(s('ML'))), cat(s('MH'), opt(s('ML'))), s('ML')), opt(s('As')))))
    db = opt(cat(s('DB'), opt(s('As'))))
    main_vowel_group = cat(star(cat(s('VPre'), opt(s('VS')))), star(s('VAbv')), star(s('VBlw')), star(s('A')), db)
    post_vowel_group = cat(s('VPst'), opt(s('MH')), opt(s('ML')), star(s('As')), star(s('VAbv')), star(s('A')), db)
    tone_group = alt(sm, cat(s('PT'), star(s('A')), opt(s('DB')), opt(s('As'))))
    complex_syllable_tail = cat(star(s('As')), medial_group, main_vowel_group, star(post_vowel_group), star(tone_group), opt(j))
    syllable_tail = cat(star(cat(s('H'), alt(c, s('IV')), opt(s('VS')))), alt(s('H'), complex_syllable_tail))
    consonant_syllable = cat(opt(alt(k, s('CS'))), alt(c, s('IV'), s('GB'), s('DOTTEDCIRCLE')), opt(s('VS')), syllable_tail)
    broken_cluster = cat(opt(k), opt(s('VS')), syllable_tail)
    patterns = [(consonant_syllable, 1), (alt(j, s('SMPst')), 3), (broken_cluster, 2), (g.any(), 3)]
    return compressed('myanmar', 'hb-ot-shaper-myanmar-machine.rl', g.categories, patterns,
                      '1 consonant syllable, 2 broken cluster, 3 non-Myanmar cluster')


# U order in src/text/scripts/syllabic_shape.cpp. CGJ never reaches the scanner.
USE_CATEGORIES = ['O', 'B', 'N', 'GB', 'CGJ', 'SUB', 'H', 'HN', 'ZWNJ', 'WJ', 'R', 'CS', 'IS', 'Sk', 'G', 'J', 'SB', 'SE',
                  'HVM', 'HM', 'HR', 'RK', 'FAbv', 'FBlw', 'FPst', 'MAbv', 'MBlw', 'MPst', 'MPre', 'CMAbv', 'CMBlw',
                  'VAbv', 'VBlw', 'VPst', 'VPre', 'VMAbv', 'VMBlw', 'VMPst', 'VMPre', 'SMAbv', 'SMBlw', 'FMAbv', 'FMBlw',
                  'FMPst']


def use():
    g = Grammar(USE_CATEGORIES)
    s = g.sym
    h = s('H', 'HVM', 'IS', 'Sk')
    consonant_modifiers = cat(star(s('CMAbv')), star(s('CMBlw')),
                              star(cat(alt(cat(h, s('B')), s('SUB')), star(s('CMAbv')), star(s('CMBlw')))))
    medial_consonants = cat(opt(s('MPre')), opt(s('MAbv')), opt(s('MBlw')), opt(s('MPst')))
    dependent_vowels = alt(cat(star(s('VPre')), star(s('VAbv')), star(s('VBlw')), star(s('VPst'))), s('H'))
    vowel_modifiers = cat(opt(s('HVM')), star(s('VMPre')), star(s('VMAbv')), star(s('VMBlw')), star(s('VMPst')))
    final_consonants = cat(star(s('FAbv')), star(s('FBlw')), star(s('FPst')))
    final_modifiers = alt(cat(star(s('FMAbv')), star(s('FMBlw'))), opt(s('FMPst')))
    complex_syllable_start = cat(opt(s('R', 'CS')), s('B', 'GB'))
    complex_syllable_middle = cat(consonant_modifiers, medial_consonants, dependent_vowels, vowel_modifiers,
                                  star(cat(s('Sk'), s('B'))))
    complex_syllable_tail = cat(complex_syllable_middle, final_consonants, final_modifiers)
    number_joiner_terminated_cluster_tail = cat(star(cat(s('HN'), s('N'))), s('HN'))
    numeral_cluster_tail = plus(cat(s('HN'), s('N')))
    symbol_cluster_tail = alt(cat(plus(s('SMAbv')), star(s('SMBlw'))), plus(s('SMBlw')))
    virama_terminated_cluster_tail = cat(consonant_modifiers, s('IS', 'RK'))
    virama_terminated_cluster = cat(complex_syllable_start, virama_terminated_cluster_tail)
    sakot_terminated_cluster_tail = cat(complex_syllable_middle, s('Sk'))
    sakot_terminated_cluster = cat(complex_syllable_start, sakot_terminated_cluster_tail)
    standard_cluster = cat(complex_syllable_start, complex_syllable_tail)
    tail = alt(complex_syllable_tail, sakot_terminated_cluster_tail, symbol_cluster_tail, virama_terminated_cluster_tail)
    broken_cluster = cat(opt(s('R')), alt(tail, number_joiner_terminated_cluster_tail, numeral_cluster_tail))
    number_joiner_terminated_cluster = cat(s('N'), number_joiner_terminated_cluster_tail)
    numeral_cluster = cat(s('N'), opt(numeral_cluster_tail))
    symbol_cluster = cat(s('O', 'GB', 'SB'), opt(tail))
    glyph = cat(s('G'), opt(s('HR')), opt(s('HM')), star(s('SE')))
    hieroglyph_cluster = cat(star(s('SB')), glyph, star(cat(s('J'), star(s('SB')), opt(glyph))))
    zwnj = opt(s('ZWNJ'))
    # Outcome: the HarfBuzz use_syllable_type_t value plus one.
    patterns = [(cat(virama_terminated_cluster, zwnj), 1), (cat(sakot_terminated_cluster, zwnj), 2),
                (cat(standard_cluster, zwnj), 3), (cat(number_joiner_terminated_cluster, zwnj), 4),
                (cat(numeral_cluster, zwnj), 5), (cat(symbol_cluster, zwnj), 6), (cat(hieroglyph_cluster, zwnj), 7),
                (s('FMPst'), 9), (cat(broken_cluster, zwnj), 8), (g.any(), 9)]
    return compressed('use', 'hb-ot-shaper-use-machine.rl', g.categories, patterns,
                      'the syllable type plus one (1 virama-terminated, 2 sakot-terminated, 3 standard, 4 number-joiner-'
                      'terminated, 5 numeral, 6 symbol, 7 hieroglyph, 8 broken, 9 non-cluster)')


def main():
    for build in (indic, khmer, myanmar, use):
        name, lines = build()
        (ROOT / name).write_text('\n'.join(lines) + '\n', encoding='utf-8', newline='\n')


if __name__ == '__main__':
    main()
