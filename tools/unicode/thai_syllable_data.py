"""Normalize the pinned Windows Thai fallback grammar into a stack-free machine.

Generation/QA only. The output is Windows-derived compatibility data, not an
independently authored linguistic grammar. No DLL or private call is used by
the native runtime.
"""
import argparse
from collections import Counter
import ctypes
import hashlib
import json
from pathlib import Path
import struct

from thai_selection_probe import Reference
from thai_unknown_probe import DLL_SHA256


def machine():
    reference = Reference()
    base = reference.dll._handle
    def table(rva, count, fmt='B'):
        return struct.unpack('<' + str(count) + fmt,
                             ctypes.string_at(base + rva, count * struct.calcsize(fmt)))
    starts = table(0x417120, 80, 'H')
    ends = table(0x416c50, 79, 'H')
    symbols = table(0x416f20, 508)
    ops = table(0x416d20, 508)
    params = table(0x4171c0, 508)
    lhs = table(0x416cf0, 41)
    pops = table(0x416c18, 41)
    semantics = table(0x4174c0, 8)
    tokens = table(0x4173c0, 256)
    classes = table(0x394b20, 256, 'i')
    alphabet = sorted(set(tokens))
    assert starts[-1] == 508
    assert [(s, symbols[starts[s]:ends[s]]) for s in (6, 7, 33)] == [(6, (3,)), (7, (3,)), (33, (3,))]
    assert all(3 not in symbols[starts[s]:ends[s]] for s in range(79) if s not in (6, 7, 33))
    assert pops[4] == 0 and lhs[4] == 6
    assert pops[6:8] == (2, 2) and lhs[6:8] == (1, 1) and semantics[6:8] == (1, 2)
    for state, production in ((6, 7), (7, 6)):
        assert (ops[ends[state]], params[ends[state]]) == (5, 4)
        assert (symbols[starts[state+1]-1], ops[starts[state+1]-1], params[starts[state+1]-1]) == (6, 2, production)
    assert (symbols[ends[0]+1], ops[ends[0]+1]) == (1, 0)

    def outcome(state, token, visited=()):
        assert state not in visited and state not in (6, 7, 33)
        first, end = starts[state], ends[state]
        index = next((i for i in range(first, end) if symbols[i] == token), end)
        op, param = ops[index], params[index]
        if op == 1:  # Consuming transition.
            return param
        if op == 7:  # No error transition exists on the unfinished syllable stack.
            return -1
        if op == 3:  # Shift and complete the only syllable.
            assert lhs[param] in (2, 7)
            return -2 if lhs[param] == 2 else -3
        if op == 8:  # Empty optional tone, fused with its goto.
            assert [(symbols[i], ops[i], params[i]) for i in range(end + 1, starts[state + 1])] == [(12, 1, param)]
            return outcome(param, token, visited + (state,))
        assert op == 5
        if lhs[param] == 7:  # Complete without consuming the lookahead.
            return -4
        assert lhs[param] == 12
        if param == 11:
            # State 19 is entered only by a tone after state 3. Reduce that
            # one token and follow state 3's optional-tone goto, state 18.
            assert state == 19
            return outcome(18, token, visited + (state,))
        assert param == 10  # Empty tone followed by a fused syllable reduction.
        goto = [(symbols[i], ops[i], params[i]) for i in range(end + 1, starts[state + 1])]
        assert len(goto) == 1 and goto[0][:2] == (12, 3) and lhs[goto[0][2]] in (2, 7)
        return -4 if lhs[goto[0][2]] == 7 else -5

    # Reachability and minimization operate on observable consuming/terminal
    # outcomes, after zero-width grammar operations have been folded away.
    rows, pending = {}, [0]
    for state in pending:
        row = tuple(outcome(state, token) for token in alphabet)
        rows[state] = row
        for target in row:
            if target >= 0 and target not in pending:
                pending.append(target)
    assert all(s not in (6, 7, 33) for s in rows)
    assert [(s, alphabet[i]) for s, row in rows.items() for i, v in enumerate(row) if v == 19] == [(3, 9)]
    partitions = {s: 0 for s in rows}
    while True:
        signatures, following = {}, {}
        for state, row in rows.items():
            signature = tuple(partitions[v] if v >= 0 else v for v in row)
            following[state] = signatures.setdefault(signature, len(signatures))
        if following == partitions:
            break
        partitions = following
    reduced = [None] * (max(partitions.values()) + 1)
    for state, row in rows.items():
        reduced[partitions[state]] = tuple(partitions[v] if v >= 0 else v for v in row)
    assert partitions[0] == 0
    # Merge character classes that have identical behavior in every state.
    columns, token_classes = {}, {}
    for index, token in enumerate(alphabet):
        column = tuple(row[index] for row in reduced)
        token_classes[token] = columns.setdefault(column, len(columns))
    matrix = [tuple(column[state] for column in columns) for state in range(len(reduced))]
    def depth(state, visited=()):
        assert state not in visited, 'Unexpected consuming cycle'
        return max(1 + depth(v, visited + (state,)) if v >= 0 else int(v in (-2, -3))
                   for v in set(matrix[state]))
    maximum = depth(0)
    assert maximum == 6
    mapping = [token_classes[t] for t in tokens]
    return dict(matrix=matrix, mapping=mapping, odd=[v & 1 for v in classes],
                reachable_states=len(rows), original_states=79,
                partitions=partitions, alphabet=alphabet, rows=rows, maximum_consumed=maximum)


def raw_result(data, model):
    state = position = 0
    while True:
        byte = data[position] if position < len(data) else 0
        target = model['matrix'][state][model['mapping'][byte]]
        if target >= 0:
            position += 1
            state = target
        elif target == -1:
            return 0
        elif target == -2:
            return position + 2
        elif target == -5:
            return position + 1
        else:
            return -(position + (2 if target == -3 else 1))


def helper_result(data, model):
    if len(data) > 13:
        for index in range(10, 0, -1):
            if data[index] == 0xec:
                extra = 0xa1 <= data[index + 1] <= 0xce and 0xe0 <= data[index + 2] <= 0xe4
                return index + 2 + extra
    result = raw_result(data, model)
    if result >= 0:
        return result
    count = -result
    previous = data[count - 1] if count - 1 < len(data) else 0
    if 0xe0 <= previous <= 0xe4:
        return count
    if not 0xa1 <= previous <= 0xce:
        return 0
    following = data[count] if count < len(data) else 0
    if not model['odd'][following]:
        return count
    return count + 1 if 0xe0 <= following <= 0xe4 else 0


def emit(model):
    rows, edges = [], []
    for row in model['matrix']:
        default = Counter(row).most_common(1)[0][0]
        rows.append(len(edges))
        edges.extend((default & 255, sum(value != default for value in row)))
        for token, value in enumerate(row):
            if value != default:
                edges.extend((token, value & 255))
    def array(name, kind, values):
        lines = [f'static constexpr {kind} {name}[] = {{']
        lines.extend('    ' + ','.join(str(v) for v in values[i:i+20]) + ',' for i in range(0, len(values), 20))
        return '\n'.join(lines + ['};'])
    # Outside Thai, runtime admits only space/ZWSP and the bounded end sentinel.
    # Their token classes coincide here; no generic byte-text parser is exposed.
    assert model['mapping'][0] == model['mapping'][32]
    assert model['odd'][0] == model['odd'][32] == 1
    mapping = model['mapping'][0xa0:0xfc]
    odd = [sum(model['odd'][0xa0 + j * 8 + bit] << bit for bit in range(8)) for j in range(12)]
    body = '// Generated by tools/unicode/thai_syllable_data.py.\n// Windows-derived compatibility data; generation-only DLL inspection.\n'
    body += f'static constexpr u8 thai_syllable_other = {model["mapping"][0]};\n'
    body += '\n'.join((array('thai_syllable_classes','u8',mapping),array('thai_syllable_odd','u8',odd),
                      array('thai_syllable_rows','u16',rows),array('thai_syllable_edges','u8',edges))) + '\n'
    return body, len(mapping) + len(odd) + len(rows)*2 + len(edges) + 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--report', required=True, type=Path)
    args = parser.parse_args()
    model = machine()
    body, size = emit(model)
    args.output.write_text(body, encoding='utf-8')
    report = dict(dll_sha256=DLL_SHA256, generator_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                  source_sha256=hashlib.sha256(args.output.read_bytes()).hexdigest(),
                  original_states=model['original_states'], reachable_states=model['reachable_states'],
                  states=len(model['matrix']), classes=len(model['matrix'][0]), payload_bytes=size,
                  maximum_consumed=model['maximum_consumed'])
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
