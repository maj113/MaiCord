"""Offline unknown-fragment recovery experiments; never imported by the engine.

Use observed lexical data to compare explicit hypotheses about resynchronizing
after unknown text. No model is selected or published to runtime by this tool.
"""
import argparse
import csv
import itertools
import json
import math
from pathlib import Path
import time

from thai_token_supplement import baseline_trie


def observations(probe, log, deadline=float('inf')):
    import struct
    data = probe.read_bytes()
    if data[:8] != b'MCLBPRB1':
        raise ValueError('Invalid probe')
    total, = struct.unpack_from('<I', data, 8)
    at = 12
    records = [r for r in csv.DictReader(log.open(encoding='utf-8'))
               if r['source_line'].isdigit()]
    if len(records) != total:
        raise ValueError('Incomplete observation rows')
    result = []
    for identifier, row in enumerate(records, 1):
        if identifier % 256 == 0 and time.monotonic() > deadline:
            raise TimeoutError('Observation parsing deadline exceeded')
        source, count = struct.unpack_from('<II', data, at)
        at += 8
        if not 0 < count <= 256:
            raise ValueError('Probe source length out of bounds')
        text = ''.join(map(chr, struct.unpack_from('<' + 'I' * count, data, at)))
        at += count * 4
        if source != identifier or int(row['source_line']) != source or int(row['scalars']) != count:
            raise ValueError('Mismatched observation identity')
        values = {}
        for name in ('layout', 'native'):
            vector = [int(x) for x in row[name].split()]
            if len(vector) != count + 1 or any(x not in (0, 1, 2, 3) for x in vector):
                raise ValueError('Invalid boundary vector')
            values[name] = [i for i, x in enumerate(vector) if 0 < i < count and x == 1]
        result.append(dict(text=text, reference=values['layout'], before=values['native']))
    if at != len(data):
        raise ValueError('Trailing probe bytes')
    return result


def mark(c):
    cp = ord(c)
    return cp == 0xe31 or 0xe34 <= cp <= 0xe3a or 0xe47 <= cp <= 0xe4e


def begin(c):
    cp = ord(c)
    return 0xe01 <= cp <= 0xe2e or 0xe40 <= cp <= 0xe44


class Recovery:
    def __init__(self, words, recognizers):
        self.tries = [baseline_trie(recognizers), baseline_trie(words)]

    def prepare(self, text):
        tables = []
        for trie in self.tries:
            rows = []
            for i in range(len(text)):
                node, ends, prefix = trie, [], 0
                for j in range(i, min(len(text), i + 31)):
                    node = node.get(text[j])
                    if node is None:
                        break
                    prefix = j + 1 - i
                    if None in node:
                        ends.append(j + 1)
                rows.append((ends, prefix))
            rows.append(([], 0))
            tables.append(rows)
        return tables

    @staticmethod
    def predict(text, tables, baseline, options):
        lexical, recover, combine, prefix_limit, minimum_unknown, safe = options
        matches = tables[lexical]
        def ends(i):
            return [j for j in matches[i][0] if not safe or j == len(text) or not mark(text[j])]
        output = set(baseline)
        i = 0
        while i < len(text):
            if not (0xe00 <= ord(text[i]) < 0xe80 or text[i] == ' '):
                i += 1
                continue
            limit = i + 1
            while limit < len(text) and (0xe00 <= ord(text[limit]) < 0xe80 or text[limit] == ' '):
                limit += 1
            output.difference_update(range(i + 1, limit))
            while i < limit:
                if text[i] == ' ':
                    i += 1
                    if i < limit and text[i] != ' ':
                        output.add(i)
                    continue
                choices = ends(i)
                selected = choices[-1] if choices else i
                # Prefer a complete three-unit path, preserving the longest
                # first unit. If none exists, a two-unit path remains a candidate.
                for one in reversed(choices):
                    following = ends(one)
                    if one == limit:
                        selected = one
                        break
                    if following:
                        selected = one
                        if any(two == limit or ends(two) for two in following):
                            break
                length = selected - i
                if length < combine and selected < limit and not ends(selected) and (
                        not length or matches[selected][1] < prefix_limit):
                    scan = selected + 1
                    while scan < limit:
                        preceding = ord(text[scan - 1])
                        possible = (0xe00 <= preceding < 0xe80 and preceding != 0xe31
                                    and not 0xe40 <= preceding <= 0xe44 and begin(text[scan]))
                        if scan - selected >= minimum_unknown and possible and tables[recover][scan][0]:
                            break
                        scan += 1
                    selected = scan
                if selected == i:
                    selected += 1
                while selected < limit and (mark(text[selected]) or text[selected] == ' '):
                    selected += 1
                if selected < limit and text[selected] in '\u0e2f\u0e46' and not ends(selected):
                    if text[selected - 1] != text[selected]:
                        selected += 1
                if selected < len(text):
                    output.add(selected)
                i = selected
        return sorted(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('words', 'recognizers', 'probe', 'observations', 'output'):
        parser.add_argument('--' + name, required=True, type=Path)
    parser.add_argument('--options', type=int, nargs=6,
                        help='Fixed lexical/recovery lexicons, combine/prefix thresholds, minimum scan, safe endpoint')
    parser.add_argument('--deadline', type=float, default=5)
    args = parser.parse_args()
    if not math.isfinite(args.deadline) or args.deadline <= 0:
        parser.error('--deadline must be finite and positive')
    started = time.monotonic()
    deadline = started + args.deadline
    model = Recovery(args.words.read_text(encoding='utf-8').splitlines(),
                     args.recognizers.read_text(encoding='utf-8').splitlines())
    rows = observations(args.probe, args.observations, deadline)
    prepared = []
    for row in rows:
        if time.monotonic() > deadline:
            raise TimeoutError('Model preparation deadline exceeded')
        prepared.append((row, model.prepare(row['text'])))
    candidates = [args.options] if args.options else itertools.product(range(2), range(2), (2, 3, 4), (2, 3, 4), (1, 2, 3), range(2))
    summary = []
    for options in candidates:
        if time.monotonic() - started > args.deadline:
            raise TimeoutError('Recovery model deadline exceeded; no partial success report')
        exact = improved = regressed = 0
        for row, table in prepared:
            if time.monotonic() > deadline:
                raise TimeoutError('Model comparison deadline exceeded')
            prediction = model.predict(row['text'], table, row['before'], options)
            good, was_good = prediction == row['reference'], row['before'] == row['reference']
            exact += good
            improved += good and not was_good
            regressed += was_good and not good
        summary.append(dict(options=options, exact=exact, improvements=improved, regressions=regressed))
    summary.sort(key=lambda r: (-r['exact'], r['regressions'], r['options']))
    report = dict(cases=len(rows), baseline_exact=sum(r['before'] == r['reference'] for r in rows),
                  seconds=time.monotonic() - started, runtime_modified=False, candidates=summary)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps({**report, 'candidates': summary[:4]}))


if __name__ == '__main__':
    main()
