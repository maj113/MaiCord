"""Generate the Thai orthographic syllable machine used for unknown-word recovery.

The grammar below is first-party, written from the standard Thai vowel
spellings (an optional leading vowel, one or two initial consonants, vowel
signs and tone marks in Unicode order, an optional final consonant) and needs
no input data. It recognizes one syllable from a starting position with one
scalar of lookahead and reports how it ended:

- closed: the syllable is complete (a short vowel such as SARA A, a vowel
  that includes its final such as SARA AM, or a consumed final consonant);
- open: a following consonant may still be this syllable's final, which
  native_thai_unknown_recovery decides from the scalar after it;
- fail: no syllable starts here, or it is malformed.

Each state may consume the scalar (a transition) or finish before or
including it. The machine is acyclic, so recognition reads a bounded number of
scalars. It is minimized, its character classes merged where they behave
identically, and the emitted tables are decoded again and compared with the
grammar before the output is written.

    python -I tools/unicode/thai_syllable_data.py [--output <file>]
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path

OUTPUT = Path(__file__).resolve().parents[2] / 'src/text/generated/thai_syllable_data.inc'

# Character classes over U+0E00..U+0E5B; anything else, including the end of
# the span, is 'other'.
CLASSES = {
    'consonant': [c for c in range(0xe01, 0xe2f) if c not in (0xe22, 0xe23, 0xe24, 0xe25, 0xe26, 0xe27, 0xe2d)],
    'cluster': [0xe23, 0xe25],  # RO RUA, LO LING: second consonant of a cluster
    'wo': [0xe27],               # WO WAEN: cluster consonant and part of the vowel -UA
    'yo': [0xe22],               # YO YAK: part of the vowel -IA
    'o': [0xe2d],                # O ANG: vowel carrier in -O, -UEA, -OE
    'ru': [0xe24, 0xe26],        # RU, LU: vowel letters
    'e': [0xe40],                # SARA E
    'ae_o': [0xe41, 0xe42],      # SARA AE, SARA O
    'ai': [0xe43, 0xe44],        # SARA AI MAIMUAN, SARA AI MAIMALAI
    'a': [0xe30],                # SARA A
    'han': [0xe31],              # MAI HAN-AKAT
    'aa': [0xe32],               # SARA AA
    'am': [0xe33],               # SARA AM
    'i': [0xe34],                # SARA I
    'ii': [0xe35],               # SARA II
    'ue': [0xe36],               # SARA UE
    'uee': [0xe37],              # SARA UEE
    'u': [0xe38, 0xe39],         # SARA U, SARA UU
    'phinthu': [0xe3a],
    'lakkhangyao': [0xe45],
    'taikhu': [0xe47],           # MAITAIKHU
    'tone': [0xe48, 0xe49, 0xe4a, 0xe4b],
    'silent': [0xe4c],           # THANTHAKHAT
    'nikhahit': [0xe4d, 0xe4e],  # NIKHAHIT, YAMAKKAN
}
CONSONANTS = ('consonant', 'cluster', 'wo', 'yo', 'o')
NOT_O = ('consonant', 'cluster', 'wo', 'yo')
BREAK = ('e', 'ae_o', 'ai', 'other')  # A leading vowel or non-Thai scalar starts something new.
OUTCOMES = {'closed': 251, 'open': 252, 'open+': 253, 'closed+': 254, 'fail': 255}
# A trailing '+' consumes the scalar that finishes the syllable.

# state: ({class or class group: target}, target for every other class)
GRAMMAR = {
    'start': ({CONSONANTS: 'initial', 'ru': 'ru', 'e': 'e', 'ae_o': 'ae_o', 'ai': 'ai'}, 'fail'),
    'ru': ({'lakkhangyao': 'closed+'}, 'closed'),
    # One initial consonant. Without a vowel sign it is a syllable with an
    # inherent vowel, whose final the recovery decides.
    'initial': ({('consonant', 'cluster', 'yo'): 'initials', 'wo': 'initial_wo', 'o': 'vowel_o', 'tone': 'toned',
                 'a': 'closed+', 'am': 'closed+', 'aa': 'vowel_aa', ('i', 'ii', 'ue', 'u'): 'vowel',
                 'uee': 'vowel_uee', 'han': 'han',
                 'taikhu': 'taikhu', 'silent': 'closed+', 'nikhahit': 'nikhahit', 'phinthu': 'closed+',
                 'lakkhangyao': 'closed+', BREAK: 'open'}, 'fail'),
    # Two consonants: a cluster when vowel signs follow, else an inherent
    # vowel and a final.
    'initials': ({'o': 'vowel_o', 'tone': 'toned', 'a': 'closed+', 'am': 'closed+', 'aa': 'vowel_aa',
                  ('i', 'ii', 'ue', 'u'): 'vowel', 'uee': 'vowel_uee', 'han': 'han', 'taikhu': 'taikhu',
                  'silent': 'closed+', NOT_O: 'closed', BREAK: 'closed'}, 'fail'),
    # Consonant and WO WAEN: a cluster before vowel signs, else the vowel -UA
    # spelled without MAI HAN-AKAT before a final.
    'initial_wo': ({'o': 'vowel_o', 'tone': 'toned', 'a': 'closed+', 'am': 'closed+', 'aa': 'vowel_aa',
                    ('i', 'ii', 'ue', 'u'): 'vowel', 'uee': 'vowel_uee', 'han': 'han', 'taikhu': 'taikhu',
                    'silent': 'closed+', NOT_O: 'final', BREAK: 'closed'}, 'fail'),
    'toned': ({'aa': 'vowel_aa', 'a': 'closed+', 'am': 'closed+', 'o': 'vowel_o', 'wo': 'vowel_wo',
               ('consonant', 'cluster', 'yo'): 'final', BREAK: 'closed'}, 'fail'),
    'vowel_wo': ({CONSONANTS: 'final', 'silent': 'closed+', BREAK: 'closed'}, 'fail'),
    'vowel_o': ({CONSONANTS: 'open', BREAK: 'open', 'silent': 'closed+'}, 'fail'),
    'vowel': ({'tone': 'vowel_toned', CONSONANTS: 'open', BREAK: 'open', 'silent': 'closed+'}, 'fail'),
    'vowel_toned': ({CONSONANTS: 'open', BREAK: 'open', 'silent': 'closed+'}, 'fail'),
    'vowel_uee': ({'tone': 'vowel_uee_toned', 'o': 'closed+', NOT_O: 'final'}, 'fail'),
    'vowel_uee_toned': ({'o': 'closed+', NOT_O: 'final'}, 'fail'),
    'vowel_aa': ({CONSONANTS: 'open', BREAK: 'open'}, 'fail'),
    'han': ({'tone': 'han_toned', 'wo': 'closed+', ('consonant', 'cluster', 'yo', 'o'): 'final'}, 'fail'),
    'han_toned': ({'wo': 'closed+', ('consonant', 'cluster', 'yo', 'o'): 'final'}, 'fail'),
    'taikhu': ({CONSONANTS: 'final', BREAK: 'closed'}, 'fail'),
    'nikhahit': ({'aa': 'closed+'}, 'closed'),
    'final': ({'silent': 'closed+', CONSONANTS: 'closed', BREAK: 'closed'}, 'fail'),
    # Leading vowels SARA AI MAIMUAN and MAIMALAI include their final.
    'ai': ({CONSONANTS: 'ai_initial'}, 'fail'),
    'ai_initial': ({'tone': 'ai_toned', ('cluster', 'wo'): 'ai_cluster', CONSONANTS: 'closed',
                    BREAK: 'closed'}, 'fail'),
    'ai_cluster': ({'tone': 'ai_toned', CONSONANTS: 'closed', BREAK: 'closed'}, 'fail'),
    'ai_toned': ({CONSONANTS: 'closed', BREAK: 'closed'}, 'fail'),
    # SARA AE and SARA O: open, with SARA A, with MAITAIKHU and a final, or with a final.
    'ae_o': ({CONSONANTS: 'ae_o_initial'}, 'fail'),
    'ae_o_initial': ({'tone': 'ae_o_toned', 'a': 'closed+', 'taikhu': 'short_final', CONSONANTS: 'ae_o_initials',
                      BREAK: 'open'}, 'fail'),
    'ae_o_toned': ({'a': 'closed+', CONSONANTS: 'open', BREAK: 'open'}, 'fail'),
    'ae_o_initials': ({'tone': 'ae_o_toned', 'a': 'closed+', 'taikhu': 'short_final', 'silent': 'closed+',
                       CONSONANTS: 'closed', BREAK: 'closed'}, 'fail'),
    'short_final': ({CONSONANTS: 'final'}, 'fail'),
    # SARA E spellings: -E, -E with SARA A, -AO, -AW (with SARA A), -OE,
    # -OE with a final (SARA I), -IA and -UEA (each optionally with SARA A).
    'e': ({CONSONANTS: 'e_initial'}, 'fail'),
    'e_initial': ({'tone': 'e_toned', 'a': 'closed+', 'aa': 'e_aa', 'ii': 'e_ii', 'uee': 'e_uee',
                   'i': 'e_i', 'taikhu': 'short_final', 'o': 'e_o', NOT_O: 'e_initials', BREAK: 'open'}, 'fail'),
    'e_initials': ({'tone': 'e_toned', 'a': 'closed+', 'aa': 'e_aa', 'ii': 'e_ii', 'uee': 'e_uee',
                    'i': 'e_i', 'taikhu': 'short_final', 'o': 'e_o', 'silent': 'closed+',
                    NOT_O: 'closed', BREAK: 'closed'}, 'fail'),
    'e_toned': ({'a': 'closed+', 'aa': 'e_aa', 'o': 'e_o', NOT_O: 'open', BREAK: 'open'}, 'fail'),
    'e_aa': ({'a': 'closed+', CONSONANTS: 'closed', BREAK: 'closed'}, 'fail'),
    'e_ii': ({'tone': 'e_ii_toned', 'yo': 'e_ia'}, 'fail'),
    'e_ii_toned': ({'yo': 'e_ia'}, 'fail'),
    'e_uee': ({'tone': 'e_uee_toned', 'o': 'e_ia'}, 'fail'),
    'e_uee_toned': ({'o': 'e_ia'}, 'fail'),
    'e_ia': ({'a': 'closed+', CONSONANTS: 'open', BREAK: 'open'}, 'fail'),
    'e_i': ({'tone': 'e_i_toned', CONSONANTS: 'final'}, 'fail'),
    'e_i_toned': ({CONSONANTS: 'final'}, 'fail'),
    'e_o': ({'a': 'closed+', CONSONANTS: 'open', BREAK: 'open'}, 'fail'),
}

# (text, expected native_thai_syllable_result) examples; results are
# consumed length plus one, negative when the syllable is open.
EXAMPLES = [
    ('กะ', 3), ('กา', -3), ('กาน', -3), ('กัน', 4), ('กัน ', 4), ('ตัว', 4), ('ตั๋ว', 5), ('คือ', 4),
    ('ชื่อ', 5), ('คืน', 4), ('ค่ำ', 4), ('คน', 3), ('คนไ', 3), ('ปลา', -4), ('ก็', 3), ('เก', -3),
    ('เกาะ', 5), ('เขา', 4), ('เรียน', -5), ('เมือง', -5), ('เกิด', 5), ('เป็น', 5), ('แข็ง', 5),
    ('แกง', 4), ('โต', -3), ('ไป', 3), ('ใคร', 4), ('ไม่', 4), ('ฤๅ', 3), ('ฤดู', 2), ('ร์', 3),
    ('กํา', 4), ('ิก', 0), ('เ', 0), ('', 0), ('ก', -2), ('ข้า', -4), ('เกี๊ยะ', 7), ('ควร', 4),
    ('ด่วน', 5), ('ขวา', -4), ('เปลี่ยน', -7), ('เดือน', -5), ('ก่อน', -4),
]


def classes():
    table = {cp: 'other' for cp in range(0xe00, 0xe5c)}
    for name, members in CLASSES.items():
        for cp in members:
            if table[cp] != 'other':
                raise ValueError(f'U+{cp:04X} is in two classes')
            table[cp] = name
    return table


def expand():
    """Resolve each reachable state to {class: target}, rejecting cycles."""
    names = [*CLASSES, 'other']
    rows = {}
    for state, (transitions, default) in GRAMMAR.items():
        row = dict.fromkeys(names, default)
        seen = set()
        # A class named alone may appear once; a group fills only the classes
        # that earlier entries left unset.
        for keys, target in transitions.items():
            group = not isinstance(keys, str)
            for key in keys if group else (keys,):
                if key not in row or (key in seen and not group):
                    raise ValueError(f'{state}: unknown or repeated class {key}')
                if key not in seen:
                    seen.add(key)
                    row[key] = target
        for target in row.values():
            if target not in OUTCOMES and target not in GRAMMAR:
                raise ValueError(f'{state}: unknown target {target}')
        rows[state] = row
    reachable, pending = ['start'], ['start']
    while pending:
        for target in rows[pending.pop()].values():
            if target in GRAMMAR and target not in reachable:
                reachable.append(target)
                pending.append(target)
    if set(reachable) != set(GRAMMAR):
        raise ValueError(f'Unreachable states: {sorted(set(GRAMMAR) - set(reachable))}')

    def depth(state, path=()):
        if state in path:
            raise ValueError('Consuming cycle through ' + ' -> '.join(path + (state,)))
        return 1 + max((depth(t, path + (state,)) for t in set(rows[state].values()) if t in GRAMMAR), default=0)

    return {s: rows[s] for s in reachable}, names, depth('start')


def minimize(rows, names):
    partition = {s: 0 for s in rows}
    while True:
        signatures, refined = {}, {}
        for state, row in rows.items():
            key = tuple(partition[t] if t in rows else t for t in (row[n] for n in names))
            refined[state] = signatures.setdefault(key, len(signatures))
        if refined == partition:
            break
        partition = refined
    matrix = [None] * (max(partition.values()) + 1)
    for state, row in rows.items():
        matrix[partition[state]] = [partition[row[n]] if row[n] in rows else OUTCOMES[row[n]] for n in names]
    columns, merged = {}, {}
    for index, name in enumerate(names):
        merged[name] = columns.setdefault(tuple(row[index] for row in matrix), len(columns))
    compact = [[None] * len(columns) for _ in matrix]
    for state, row in enumerate(matrix):
        for index, name in enumerate(names):
            compact[state][merged[name]] = row[index]
    if partition['start'] != 0:
        raise ValueError('The start state must be state 0')
    return compact, merged


def emit(matrix, merged, table):
    rows, edges = [], []
    for row in matrix:
        default = Counter(row).most_common(1)[0][0]
        rows.append(len(edges))
        exceptions = [(c, t) for c, t in enumerate(row) if t != default]
        edges += [default, len(exceptions)]
        for pair in exceptions:
            edges += pair
    mapping = [merged[table[cp]] for cp in range(0xe00, 0xe5c)]
    if max(len(matrix), len(merged)) >= 251 or len(edges) >= 65536:
        raise ValueError('Machine exceeds the table encoding')

    def array(name, kind, values):
        lines = [f'static constexpr {kind} {name}[] = {{']
        lines += ['    ' + ','.join(str(v) for v in values[i:i + 20]) + ',' for i in range(0, len(values), 20)]
        return '\n'.join(lines + ['};'])

    text = ('// Generated by tools/unicode/thai_syllable_data.py; do not edit.\n'
            '// First-party Thai syllable grammar authored in that generator; no input data.\n'
            f'static constexpr u8 thai_syllable_other = {merged["other"]};\n')
    text += '\n'.join((array('thai_syllable_classes', 'u8', mapping), array('thai_syllable_rows', 'u16', rows),
                       array('thai_syllable_edges', 'u8', edges))) + '\n'
    return text, dict(classes=mapping, rows=rows, edges=edges, other=merged['other'],
                      payload_bytes=len(mapping) + 2 * len(rows) + len(edges) + 1)


def run(tables, text):
    """Mirror of native_thai_syllable_result over the emitted tables."""
    state = position = 0
    while True:
        cp = ord(text[position]) if position < len(text) else 0
        token = tables['classes'][cp - 0xe00] if 0xe00 <= cp < 0xe5c else tables['other']
        row = tables['rows'][state]
        target = tables['edges'][row]
        for i in range(tables['edges'][row + 1]):
            if tables['edges'][row + 2 + i * 2] == token:
                target = tables['edges'][row + 3 + i * 2]
                break
        if target < 251:
            state, position = target, position + 1
        elif target == 255:
            return 0
        elif target in (251, 254):
            return position + (2 if target == 254 else 1)
        else:
            return -(position + (2 if target == 253 else 1))


def verify(rows, names, table, tables):
    """Compare every reachable state and class of the tables with the grammar."""
    def grammar(text):
        state = 'start'
        for position in range(len(text) + 1):
            cp = ord(text[position]) if position < len(text) else 0
            target = rows[state][table.get(cp, 'other')]
            if target in rows:
                state = target
                continue
            return {'fail': 0, 'closed': position + 1, 'closed+': position + 2,
                    'open': -(position + 1), 'open+': -(position + 2)}[target]
        raise AssertionError('unreachable')

    representatives = {name: chr(next(cp for cp, n in sorted(table.items()) if n == name)) if name != 'other' else ' '
                       for name in names}
    witnesses, pending = {'start': ''}, ['start']
    while pending:
        state = pending.pop(0)
        for name in names:
            target = rows[state][name]
            if target in rows and target not in witnesses:
                witnesses[target] = witnesses[state] + representatives[name]
                pending.append(target)
    checked = 0
    for state, prefix in witnesses.items():
        for name in names:
            for cp in [cp for cp, n in table.items() if n == name] if name != 'other' else [0x20, 0x41]:
                sample = prefix + chr(cp)
                if run(tables, sample) != grammar(sample):
                    raise ValueError(f'Emitted tables differ from the grammar at state {state}')
                checked += 1
    for text, expected in EXAMPLES:
        if grammar(text) != expected or run(tables, text) != expected:
            raise ValueError(f'Example {text!r}: expected {expected}, got {grammar(text)}')
    return checked


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--output', type=Path, default=OUTPUT)
    args = parser.parse_args()
    table = classes()
    rows, names, depth = expand()
    matrix, merged = minimize(rows, names)
    text, tables = emit(matrix, merged, table)
    checked = verify(rows, names, table, tables)
    args.output.write_bytes(text.encode('utf-8'))
    print(json.dumps(dict(grammar_states=len(rows), states=len(matrix), classes=len(set(merged.values())),
                          longest_consumed=depth, payload_bytes=tables['payload_bytes'], verified_cases=checked,
                          examples=len(EXAMPLES),
                          generator_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                          output_sha256=hashlib.sha256(args.output.read_bytes()).hexdigest())))


if __name__ == '__main__':
    main()
