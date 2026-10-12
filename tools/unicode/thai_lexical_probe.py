"""Freeze private-reference lexical answers for independent native replay."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import time

from thai_selection_probe import Reference, engine_spans, inputs
from thai_unknown_probe import DLL_SHA256


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--fixture', type=Path, required=True)
    parser.add_argument('--start', type=int, default=1)
    parser.add_argument('--count', type=int, default=4096)
    args = parser.parse_args()
    if args.start < 1 or args.count < 1:
        parser.error('Start/count must be positive')
    if args.output.exists() or args.fixture.exists():
        raise ValueError('Evidence already exists')
    started = time.perf_counter()
    reference = Reference()
    payload = bytearray(b'MCTHLEX2' + bytes(4))
    sources = spans = queries = negative = selections = 0
    for identifier, source in inputs(args.probe):
        if identifier < args.start:
            continue
        if identifier >= args.start + args.count:
            break
        sources += 1
        for offset, text in engine_spans(source):
            reference.prepare(text)
            spans += 1
            payload += struct.pack('<I', len(text))
            payload += struct.pack('<' + 'I' * len(text), *map(ord, text))
            for position in range(len(text)):
                values = reference.lookup(position)
                payload += struct.pack('<8i', *(values + (0,) * (8 - len(values))))
                queries += 1
                negative += any(v < 0 for v in values)
            selection_count_at = len(payload)
            payload += bytes(4)
            position = count = 0
            while position < len(text):
                output, retained, total = reference.selection(position)
                payload += struct.pack('<II', position, total)
                for values in (output, retained):
                    payload += struct.pack('<8i', *(values + (0,) * (8 - len(values))))
                position += max(output[0], 1)
                count += 1
            struct.pack_into('<I', payload, selection_count_at, count)
            selections += count
    struct.pack_into('<I', payload, 8, spans)
    args.fixture.parent.mkdir(parents=True, exist_ok=True)
    args.fixture.write_bytes(payload)
    result = dict(status='captured', source_inputs=sources, spans=spans, queries=queries,
                  negative_queries=negative, selections=selections, seconds=time.perf_counter() - started,
                  input=dict(path=str(args.probe.resolve()), sha256=hashlib.sha256(args.probe.read_bytes()).hexdigest()),
                  fixture=dict(path=str(args.fixture.resolve()), bytes=len(payload), sha256=hashlib.sha256(payload).hexdigest()),
                  source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                  observer_sha256=hashlib.sha256(Path(__file__).with_name('thai_selection_probe.py').read_bytes()).hexdigest(),
                  reference_dll_sha256=DLL_SHA256,
                  scope='Every position in Thai byte-alphabet spans; not public itemization or full segmentation.')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({k: result[k] for k in ('status', 'source_inputs', 'spans', 'queries', 'seconds')}))


if __name__ == '__main__':
    main()
