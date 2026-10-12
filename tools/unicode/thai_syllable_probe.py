"""Bounded, pinned Thai syllable observations and stack-free model comparison."""
import argparse
import ctypes
import hashlib
import json
from pathlib import Path
import struct
import time

from thai_selection_probe import Reference, inputs, engine_spans
from thai_syllable_data import machine, raw_result, helper_result
from thai_unknown_probe import UNKNOWN_RVA, PREFIX, encode, DLL_SHA256


def grammar_cases(model):
    # Reach each original consuming state by its shortest witness. Then cover
    # every byte admitted by the adapter and all pairs of post-boundary classes.
    reference = Reference()
    tokens = ctypes.string_at(reference.dll._handle + 0x4173c0, 256)
    representatives = {token: next(i for i, t in enumerate(tokens) if t == token) for token in model['alphabet']}
    witnesses = {0: b''}
    queue = [0]
    for state in queue:
        for token, target in zip(model['alphabet'], model['rows'][state]):
            if target >= 0 and target not in witnesses:
                witnesses[target] = witnesses[state] + bytes([representatives[token]])
                queue.append(target)
    assert len(witnesses) == model['reachable_states']
    tails = (b'', b' ', b'\xa1', b'\xa1\xd2', b'\xa1\xa1', b'\xa1\xe0', b'\xe0', b'\xe4', b'\xec', b'\xa1\xec')
    seen = set()
    for prefix in witnesses.values():
        for byte in (32, *range(0xa1, 0xfc)):
            for tail in tails:
                data = prefix + bytes([byte]) + tail
                if data in seen:
                    continue
                seen.add(data)
                yield ''.join(' ' if b == 32 else chr(b - 0xa0 + 0xe00) for b in data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path)
    parser.add_argument('--grammar', action='store_true')
    parser.add_argument('--start', type=int, default=1)
    parser.add_argument('--count', type=int, default=4096)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--report', required=True, type=Path)
    args = parser.parse_args()
    assert bool(args.input) != args.grammar
    assert args.start > 0 and 0 < args.count <= 4096
    assert not args.output.exists() and not args.report.exists()
    started = time.perf_counter()
    model = machine()
    reference = Reference()
    base = reference.dll._handle
    assert ctypes.string_at(base + UNKNOWN_RVA, len(PREFIX)) == PREFIX
    assert ctypes.string_at(base + 0x1d5834, 10) == bytes.fromhex('48 89 5c 24 08 56 48 83 ec 20')
    unknown = ctypes.WINFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p)(base + UNKNOWN_RVA)
    parse = ctypes.WINFUNCTYPE(None, ctypes.c_void_p)(base + 0x1d5834)
    cases = enumerate(grammar_cases(model), 1) if args.grammar else inputs(args.input)
    observations = bytearray(b'MCTHSYL1' + bytes(4))
    sources = spans = queries = negative = positive = 0
    mismatch = []
    for identity, text in cases:
        if identity < args.start:
            continue
        if identity >= args.start + args.count:
            break
        sources += 1
        for offset, span in engine_spans(text):
            assert 0 < len(span) <= 256
            data = encode(span)
            buffer = ctypes.create_string_buffer(data, len(data) + 64)
            address = ctypes.addressof(buffer)
            observations += struct.pack('<I', len(span)) + struct.pack('<' + str(len(span)) + 'I', *map(ord, span))
            spans += 1
            for position in range(len(data) + 1):
                pcb = ctypes.create_string_buffer(0x268)
                struct.pack_into('<Q', pcb, 0x240, address + position)
                parse(pcb)
                raw = struct.unpack_from('<i', pcb, 0x25c)[0]
                observed = unknown(address + position, address + len(data))
                expected_raw = raw_result(data[position:], model)
                expected = helper_result(data[position:], model)
                if (raw, observed) != (expected_raw, expected) and len(mismatch) < 20:
                    mismatch.append(dict(identity=identity, text=span, position=position, raw=raw,
                                         observed=observed, expected_raw=expected_raw, expected=expected))
                observations += struct.pack('<ii', raw, observed)
                queries += 1
                negative += raw < 0
                positive += raw > 0
    struct.pack_into('<I', observations, 8, spans)
    args.output.write_bytes(observations)
    report = dict(status='different' if mismatch else 'passed', sources=sources, spans=spans,
                  queries=queries, negative=negative, positive=positive, mismatches=mismatch,
                  start=args.start, count=args.count, grammar=args.grammar,
                  seconds=time.perf_counter()-started, dll_sha256=DLL_SHA256,
                  model_sha256=hashlib.sha256(Path(__file__).with_name('thai_syllable_data.py').read_bytes()).hexdigest(),
                  observer_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                  input_sha256=hashlib.sha256(args.input.read_bytes()).hexdigest() if args.input else None,
                  observations_sha256=hashlib.sha256(observations).hexdigest())
    args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({k:report[k] for k in ('status','sources','spans','queries','negative','positive','seconds','mismatches')}, ensure_ascii=False))
    return bool(mismatch)


if __name__ == '__main__':
    raise SystemExit(main())
