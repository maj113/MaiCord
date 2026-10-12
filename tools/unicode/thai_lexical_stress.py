"""Deterministic lexical endpoint, conditional-prefix and random byte-span probes."""
import argparse
import hashlib
import itertools
import json
from pathlib import Path
import random
import struct

from thai_lexical_data import extract
from thai_selection_probe import Reference


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    target = args.output_dir / 'input.bin'
    if target.exists():
        raise ValueError('Stress input already exists')
    alphabet = [' ', *(chr(cp) for cp in range(0xe01, 0xe5c))]
    cases = list(alphabet)
    cases += [''.join(chars) for chars in itertools.product(alphabet, repeat=2)]
    for left, right in itertools.product(alphabet[1:], repeat=2):
        for ending in ('', '์', 'ก์'):
            cases.append('กก' + left + right + ending)
    _, _, rules, _, _, _ = extract(Reference())
    prefixes = [''.join(chr(cp + 0xe00) for cp in rule['prefix']) for rule in rules]
    for index, rule in enumerate(rules):
        prefix = prefixes[index]
        guard = chr(rule['action'] + 0xe00) if rule['action'] < 128 else ''
        for suffix in ('', ' ', guard, guard + 'ก', 'การ์ฟิลด์ดำแดง', '์', '\u200b'):
            cases.append(prefix + suffix)
        # Success may continue into the next encoded segment; failure can
        # expose its suffix as a fresh attempt. Test both and repeated success.
        for count in (2, 3, 4):
            tail = ''.join(prefixes[index:index + count])
            cases.extend((tail, tail + 'ก', tail + ' ', 'ม' + tail))
    rng = random.Random(0x54484149)
    for _ in range(4096):
        if rng.randrange(2):
            text = ''.join(rng.choice(alphabet) for _ in range(rng.randrange(1, 65)))
        else:
            text = ''.join(rng.choice(prefixes) for _ in range(rng.randrange(1, 9)))
        cases.append(text)
    cases = list(dict.fromkeys(cases))
    payload = bytearray(b'MCLBPRB1' + struct.pack('<I', len(cases)))
    for identifier, text in enumerate(cases, 1):
        if not 0 < len(text) <= 256:
            raise ValueError('Stress input exceeds fixture capacity')
        payload += struct.pack('<II', identifier, len(text))
        payload += struct.pack('<' + 'I' * len(text), *map(ord, text))
    target.write_bytes(payload)
    report = dict(cases=len(cases), bytes=len(payload), sha256=hashlib.sha256(payload).hexdigest(),
                  source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                  scope='Synthetic private lexical-alphabet coverage; no word-boundary or app acceptance.')
    (args.output_dir / 'input.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
