"""Compile Thai recognizer metadata from explicit, corpus-wide observations.

The prefix probe is empirical evidence, not proof of the Windows dictionary.
No held-out phrases are read and no runtime platform service/library is used.
"""
import argparse
import csv
import hashlib
import json
import struct
from pathlib import Path

from word_dictionary import words_from


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('candidates', 'end', 'continuing', 'probe', 'observations', 'output-dir'):
        parser.add_argument('--' + name, required=True, type=Path)
    args = parser.parse_args()
    candidates = set(words_from(args.candidates))
    ending = json.loads(args.end.read_text(encoding='utf-8'))['annotations']
    continuing = json.loads(args.continuing.read_text(encoding='utf-8'))
    if set(ending) != set(continuing) or not candidates <= set(ending):
        raise ValueError('Observation vocabulary mismatch')
    data = args.probe.read_bytes()
    if len(data) < 12 or data[:8] != b'MCLBPRB1':
        raise ValueError('Invalid prefix probe')
    total, = struct.unpack_from('<I', data, 8)
    prefix = '\u0e01\u0e05\u0e06'
    at, seen, recognized = 12, set(), set()
    with args.observations.open(encoding='utf-8') as stream:
        rows = csv.DictReader(stream)
        for expected in range(1, total + 1):
            identifier, count = struct.unpack_from('<II', data, at)
            at += 8
            text = ''.join(map(chr, struct.unpack_from('<' + 'I' * count, data, at)))
            at += count * 4
            row = next(rows)
            if identifier != expected or int(row['source_line']) != expected or int(row['scalars']) != count:
                raise ValueError('Prefix probe row mismatch')
            if not text.startswith(prefix):
                raise ValueError('Unexpected recognition prefix')
            word = text[len(prefix):]
            if word not in ending or word in seen:
                raise ValueError('Unknown/duplicate probed word')
            seen.add(word)
            boundaries = [int(value) for value in row['windows'].split()]
            if len(boundaries) != count + 1 or any(value not in (0, 1, 2, 255) for value in boundaries):
                raise ValueError('Invalid reference boundary vector')
            if not ending[word] and boundaries[len(prefix)] == 1:
                recognized.add(word)
        if next(rows, None) is not None or at != len(data) or seen != set(ending):
            raise ValueError('Incomplete or trailing prefix observations')
    words = sorted(candidates | recognized)
    output = args.output_dir
    output.mkdir(parents=True, exist_ok=True)
    for name, entries in [('words', words), ('candidates', candidates), ('recognizers', recognized)]:
        (output / (name + '.txt')).write_text('\n'.join(sorted(entries)) + '\n', encoding='utf-8')
    (output / 'ending.json').write_text(json.dumps({'annotations': {w: ending[w] for w in words}}, ensure_ascii=False), encoding='utf-8')
    (output / 'continuing.json').write_text(json.dumps({w: continuing[w] for w in words}, ensure_ascii=False), encoding='utf-8')
    report = dict(probed=total, candidates=len(candidates), recognizers=len(recognized),
                  recognition_only=len(recognized - candidates), words=len(words),
                  prefix=[ord(c) for c in prefix], full_context_parity=False,
                  inputs={str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in
                          (args.candidates, args.end, args.continuing, args.probe, args.observations)})
    (output / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps({k: v for k, v in report.items() if k != 'inputs'}))


if __name__ == '__main__':
    main()
