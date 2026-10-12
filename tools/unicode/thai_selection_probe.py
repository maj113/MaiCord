"""Differential model of Thai candidate selection, with version-pinned QA calls.

The model recomputes bounded successor queries instead of retaining the Windows
search tree. Lexical lookup is still an oracle: this is not runtime segmentation.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import struct
import time

from thai_unknown_probe import DLL_SHA256


def select(position, limit, lookup, retained=()):
    roots = tuple(retained) or lookup(position)
    live = []
    sixth = None
    for index, length in enumerate(roots):
        following = position + max(length, 0)
        if length > 0 and following >= limit:
            return (length,), (), 1
        children = lookup(following)
        if index == 5:
            sixth = (following, children)
        if children:
            live.append(index)
    if not live:
        return (roots[0] if roots else 0,), (), 0
    if len(live) == 1:
        chosen = live[0]
        selected = [roots[chosen]]
    else:
        chosen = None
        selected = []
        remaining = len(live)
        for index in range(len(roots)):
            first = roots[index]
            following = position + max(first, 0)
            if index == 5 and sixth is not None:
                following, seconds = sixth
            else:
                seconds = lookup(following)
            if chosen is None and not seconds:
                continue
            for slot in range(8):
                if index == 5 and sixth is not None:
                    following, current = sixth
                else:
                    current = seconds
                if slot >= len(current):
                    break
                second = current[slot]
                third_at = following + abs(second)
                if chosen is None:
                    thirds = () if third_at >= limit else lookup(third_at)
                    if third_at < limit and len(roots) > 5:
                        sixth = (third_at, thirds)
                    if third_at >= limit or thirds:
                        chosen = index
                        selected.append(first)
                        break
                else:
                    # Already-covered two/three-unit prefixes do not add an
                    # alternative first boundary. Query only when necessary.
                    if abs(selected[0]) == abs(first) + abs(second):
                        break
                    if third_at >= limit:
                        continue
                    thirds = lookup(third_at)
                    if len(roots) > 5:
                        sixth = (third_at, thirds)
                    if thirds:
                        if abs(selected[0]) != abs(first) + abs(second) + abs(thirds[0]):
                            selected.append(first)
                        break
            remaining -= 1
            if not remaining:
                break
        if chosen is None:
            return (roots[0],), (), 0
    cached = (sixth[1] if chosen == 5 and sixth is not None else lookup(position + max(roots[chosen], 0))) if selected[0] > 0 else ()
    positive = tuple(length for length in selected if length > 0)
    return positive or (0,), cached, len(positive)


def vector(buffer, offset):
    values = struct.unpack_from('<8i', buffer, offset)
    return tuple(values[:values.index(0)]) if 0 in values else values


class Reference:
    def __init__(self):
        path = Path(os.environ['SystemRoot']) / 'System32/DWrite.dll'
        data = path.read_bytes()
        if ctypes.sizeof(ctypes.c_void_p) != 8 or hashlib.sha256(data).hexdigest() != DLL_SHA256:
            raise RuntimeError('Unsupported reference DLL or process architecture')
        self.dll = ctypes.WinDLL(str(path))
        def function(rva, prefix, result, *arguments):
            address = self.dll._handle + rva
            if ctypes.string_at(address, len(prefix)) != prefix:
                raise RuntimeError('Private entry bytes differ')
            return ctypes.WINFUNCTYPE(result, *arguments)(address)
        # Lazy initializer's symbol/RVA were recorded by CDB. The whole-DLL
        # identity is mandatory even for calls whose prologue is shared.
        initialize = ctypes.WINFUNCTYPE(ctypes.c_bool)(self.dll._handle + 0x12af40)
        if not initialize():
            raise RuntimeError('Reference dictionary initialization failed')
        self.search = function(0x1d5350, bytes.fromhex('48 89 5c 24 08 57'), None,
                               ctypes.c_void_p, ctypes.c_void_p)
        self.main = function(0x1d4fa0, bytes.fromhex('40 53 55 56 57 41 54'), ctypes.c_int,
                             ctypes.c_void_p, ctypes.c_void_p)

    def prepare(self, text):
        data = bytes(0x20 if c in (' ', '\u200b') else ord(c) - 0xe00 + 0xa0 for c in text)
        self.buffer = ctypes.create_string_buffer(data, len(data) + 64)
        self.base = ctypes.addressof(self.buffer)
        self.length = len(data)
        self.state = ctypes.create_string_buffer(0x170)
        self.lexical_state = ctypes.create_string_buffer(0x170)
        for state in (self.state, self.lexical_state):
            struct.pack_into('<QQ', state, 0, self.base + len(data), self.base + len(data) - 2)
        self.memo = {}
        self.used = set()

    def lookup(self, position):
        self.used.add(position)
        if position not in self.memo:
            if not 0 <= position < self.length:
                raise ValueError('Lookup outside input')
            node = ctypes.create_string_buffer(40)
            struct.pack_into('<Q', node, 0, self.base + position)
            self.search(ctypes.addressof(self.lexical_state), ctypes.addressof(node))
            self.memo[position] = vector(node, 8)
        return self.memo[position]

    def selection(self, position):
        count = self.main(ctypes.addressof(self.state), self.base + position)
        output = vector(self.state, 0x128)
        # A zero-count prefix can still be consumed by the caller.
        output = output[:count] if count else (struct.unpack_from('<i', self.state, 0x128)[0],)
        # The reference can leave stale private lookahead at end of input.
        # The native contract retains only candidates that can be used next.
        retained = () if position + max(output[0], 1) >= self.length else vector(self.state, 0x18)
        return output, retained, count


def inputs(path):
    data = path.read_bytes()
    if data[:8] != b'MCLBPRB1':
        raise ValueError('Unsupported input signature')
    count, = struct.unpack_from('<I', data, 8)
    at = 12
    for expected in range(1, count + 1):
        identifier, length = struct.unpack_from('<II', data, at)
        at += 8
        if identifier != expected or not 0 < length <= 256:
            raise ValueError('Invalid input record')
        yield identifier, ''.join(map(chr, struct.unpack_from('<' + 'I' * length, data, at)))
        at += length * 4
    if at != len(data):
        raise ValueError('Trailing input bytes')


def engine_spans(text):
    """Primitive-engine inputs, excluding scalars outside its byte alphabet.

    This is deliberately not a substitute for Windows line-class itemization.
    Reserved values inside the byte alphabet are additional primitive probes.
    """
    start = None
    for i, char in enumerate(text + '\0'):
        eligible = '\u0e01' <= char <= '\u0e5b' or char in (' ', '\u200b')
        if eligible and start is None:
            start = i
        elif not eligible and start is not None:
            yield start, text[start:i].replace('\u200b', ' ')
            start = None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--start', type=int, default=1)
    parser.add_argument('--count', type=int, default=4096)
    parser.add_argument('--native-fixture', type=Path,
                        help='Export observed selections and the lexical queries needed for native replay')
    args = parser.parse_args()
    if args.start < 1 or args.count < 1:
        parser.error('Start/count must be positive')
    if args.output.exists():
        raise ValueError('Observation already exists')
    started = time.perf_counter()
    reference = Reference()
    cases = checks = exact = spans = 0
    differences = []
    digest = hashlib.sha256()
    fixture = bytearray(b'MCTHSEL1' + bytes(4))
    fixture_count = 0
    def packed(values):
        return struct.pack('<8i', *(tuple(values) + (0,) * (8 - len(values))))
    for identifier, source_text in inputs(args.probe):
        if identifier < args.start:
            continue
        if identifier >= args.start + args.count:
            break
        for span_start, text in engine_spans(source_text):
            spans += 1
            reference.prepare(text)
            position, retained = 0, ()
            while position < reference.length:
                reference.used.clear()
                expected = reference.selection(position)
                actual = select(position, reference.length, reference.lookup, retained)
                good = expected == actual
                checks += 1
                exact += good
                row = dict(case=identifier, span_start=span_start, text=text, position=position,
                           roots=retained or reference.lookup(position),
                           expected=expected, actual=actual)
                digest.update(json.dumps(row, ensure_ascii=False).encode('utf-8'))
                if not good:
                    differences.append(row)
                if args.native_fixture:
                    fixture += struct.pack('<III', position, reference.length, expected[2])
                    fixture += packed(retained) + packed(expected[0]) + packed(expected[1])
                    fixture += struct.pack('<I', len(reference.used))
                    for at in sorted(reference.used):
                        fixture += struct.pack('<I', at) + packed(reference.memo[at])
                    fixture_count += 1
                # Follow the reference's deterministic first candidate, including
                # partial-prefix consumption. This tests retained lookup state;
                # it does not claim to reproduce the outer word-break controller.
                retained = expected[1]
                position += max(expected[0][0], 1)
        cases += 1
    report = dict(status='passed' if exact == checks else 'different', cases=cases, spans=spans,
                  checks=checks, exact=exact, differences=differences,
                  observation_sha256=digest.hexdigest(), seconds=time.perf_counter() - started,
                  source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                  input=dict(path=str(args.probe.resolve()), sha256=hashlib.sha256(args.probe.read_bytes()).hexdigest()),
                  start=args.start, count=args.count, reference_dll_sha256=DLL_SHA256,
                  scope='Candidate selection with reference lexical lookup. Input spans use U+0E01..U+0E5B and spaces/ZWSP; other scalars delimit probes. This does not prove public itemization, full segmentation or runtime performance.')
    if args.native_fixture:
        if args.native_fixture.exists():
            raise ValueError('Native fixture already exists')
        struct.pack_into('<I', fixture, 8, fixture_count)
        args.native_fixture.parent.mkdir(parents=True, exist_ok=True)
        args.native_fixture.write_bytes(fixture)
        report['native_fixture'] = dict(path=str(args.native_fixture.resolve()), cases=fixture_count,
                                        bytes=len(fixture), sha256=hashlib.sha256(fixture).hexdigest())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({k: report[k] for k in ('status', 'cases', 'checks', 'exact', 'seconds')}))
    return 0 if report['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
