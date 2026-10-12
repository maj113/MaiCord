"""Generate a compact compatibility lookup from a pinned local reference.

Only normalized lexical fingerprints, character endpoint flags and conditional
prefix rules are emitted. No Windows executable code or runtime calls are
included. The source DLL identity and independently verified encoding are part
of the generation report. This data is distinct from the existing ICU lexicon.
"""
import argparse
import ctypes
import hashlib
import json
from pathlib import Path
import struct

from thai_selection_probe import Reference
from thai_unknown_probe import DLL_SHA256


def variable(value):
    result = bytearray()
    while value >= 128:
        result.append((value & 127) | 128)
        value >>= 7
    result.append(value)
    return result


def read_variable(data, at):
    value = shift = 0
    while True:
        byte = data[at]
        at += 1
        value |= (byte & 127) << shift
        if byte < 128:
            return value, at
        shift += 7


def extract(reference):
    base = reference.dll._handle
    count = ctypes.c_uint32.from_address(base + 0x4bc178).value
    address = ctypes.c_void_p.from_address(base + 0x4bc190).value
    if not address or not 0 < count < 10000:
        raise ValueError('Unexpected lexical directory')
    directory = ctypes.string_at(address, (count + 1) * 8)
    rows = []
    for i in range(count):
        pair = struct.unpack_from('>H', directory, i * 8)[0]
        start = struct.unpack_from('<I', directory, i * 8 + 4)[0]
        end = struct.unpack_from('<I', directory, (i + 1) * 8 + 4)[0]
        if not (count + 1) * 8 <= start <= end < 1024 * 1024:
            raise ValueError('Unexpected lexical bounds')
        data = ctypes.string_at(address + start, end - start)
        at, groups = 0, []
        while at < len(data):
            length, = struct.unpack_from('<H', data, at)
            at += 2
            if length == 2:
                values = [0]
            else:
                size, = struct.unpack_from('<H', data, at)
                at += 2
                values = list(struct.unpack_from('<' + 'H' * size, data, at))
                at += size * 2
            if not 2 <= length <= 14 or values != sorted(set(values)):
                raise ValueError('Unexpected fingerprint group')
            groups.append((length, values))
        if at != len(data) or [n for n, _ in groups] != sorted({n for n, _ in groups}):
            raise ValueError('Invalid group topology')
        rows.append((pair, groups))
    if [p for p, _ in rows] != sorted({p for p, _ in rows}):
        raise ValueError('Invalid lexical directory order')
    classes = list((ctypes.c_int32 * 256).from_address(base + 0x394b20))
    flags = [int(value >= 2) * 2 + int(abs(value) % 2 == 1) for value in classes[160:256]]
    special_address = ctypes.c_void_p.from_address(base + 0x4bc188).value
    special_half = ctypes.c_void_p.from_address(base + 0x4bc180).value - special_address
    special = ctypes.string_at(special_address, 4096)
    special = special[:special.index(3) + 1]
    rules, prefix, half_index = [], [], None
    for offset, byte in enumerate(special):
        if offset == special_half:
            if prefix:
                raise ValueError('Special entry point is inside a prefix')
            half_index = len(rules)
        if byte == 3:
            if prefix:
                raise ValueError('Unterminated special prefix')
            break
        if byte > 128:
            prefix.append(byte - 160)
        else:
            action = 254 if byte == 124 else 255 if byte == 126 else byte - 32
            rules.append(dict(prefix=prefix, action=action))
            prefix = []
    if half_index is None:
        raise ValueError('Missing special entry point')
    return rows, flags, rules, half_index, end, len(special)


def arrays(name, kind, values, width=20):
    lines = [f'constexpr {kind} {name}[]{{']
    for at in range(0, len(values), width):
        lines.append('    ' + ','.join(str(v) for v in values[at:at + width]) + ',')
    lines.append('};')
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    reference = Reference()
    rows, flags, rules, half_index, original_bytes, original_special = extract(reference)
    payload, offsets, first = bytearray(), [], [255] * 128
    masks, bases = [], []
    previous = None
    keys = 0
    for index, (pair, groups) in enumerate(rows):
        high, low = (pair >> 8) - 160, (pair & 255) - 160
        if not 0 <= high < 128 or not 0 <= low < 128:
            raise ValueError('Non-Thai fingerprint key')
        if high != previous:
            first[high] = len(masks)
            masks.append(0)
            bases.append(index)
            previous = high
        masks[-1] |= 1 << low
        offsets.append(len(payload))
        payload += struct.pack('<H', sum(1 << (n - 2) for n, _ in groups))
        for length, values in groups:
            keys += len(values)
            if length != 2:
                payload += variable(len(values))
                payload += struct.pack('<' + 'H' * len(values), *values)
    offsets.append(len(payload))
    if len(payload) >= 65536 or len(masks) >= 255:
        raise ValueError('Compact directory capacity exceeded')
    # Decode every generated bucket, including every fingerprint and length.
    for index, (pair, expected) in enumerate(rows):
        high, low = (pair >> 8) - 160, (pair & 255) - 160
        row = first[high]
        actual_index = bases[row] + (masks[row] & ((1 << low) - 1)).bit_count()
        if actual_index != index or not masks[row] & (1 << low):
            raise ValueError('Rank directory mismatch')
        at = offsets[index]
        bitmap, = struct.unpack_from('<H', payload, at)
        at += 2
        actual = []
        for length in range(2, 15):
            if bitmap & (1 << (length - 2)):
                if length == 2:
                    values = [0]
                else:
                    size, at = read_variable(payload, at)
                    values = list(struct.unpack_from('<' + 'H' * size, payload, at))
                    at += size * 2
                actual.append((length, values))
        if actual != expected or at != offsets[index + 1]:
            raise ValueError('Fingerprint payload mismatch')
    packed_flags = [sum(flags[i + j] << (j * 2) for j in range(4)) for i in range(0, 96, 4)]
    special_data, special_starts = bytearray(), [0, 0]
    for index, rule in enumerate(rules):
        prefix = rule['prefix']
        if not prefix or len(prefix) >= 128 or any(not 0 <= c < 128 for c in prefix):
            raise ValueError('Invalid normalized prefix')
        if index == half_index:
            special_starts[1] = len(special_data)
        action = rule['action']
        if not (0 <= action < 128 or action in (254, 255)):
            raise ValueError('Invalid normalized action')
        special_data += bytes([len(prefix), action, *prefix])
    # Verify normalized rule order, metadata and all characters independently.
    at, decoded = 0, []
    while at < len(special_data):
        length, action = special_data[at:at + 2]
        prefix = list(special_data[at + 2:at + 2 + length])
        at += 2 + length
        decoded.append(dict(prefix=prefix, action=action))
    if decoded != rules:
        raise ValueError('Special rule round trip mismatch')
    masks64 = [part for mask in masks for part in (mask & ((1 << 64) - 1), mask >> 64)]
    source = '// Generated compatibility data; not executable Windows code.\n'
    source += '// Source DLL SHA-256: ' + DLL_SHA256 + '\n'
    source += '// Generated by tools/unicode/thai_lexical_data.py.\n'
    source += arrays('thai_lexical_first', 'u8', first) + '\n'
    source += arrays('thai_lexical_masks', 'u64', [str(v) + 'ull' for v in masks64], 4) + '\n'
    source += arrays('thai_lexical_bases', 'u16', bases) + '\n'
    source += arrays('thai_lexical_offsets', 'u16', offsets) + '\n'
    source += arrays('thai_lexical_flags', 'u8', packed_flags) + '\n'
    source += arrays('thai_lexical_payload', 'u8', payload) + '\n'
    source += arrays('thai_special_starts', 'u16', special_starts) + '\n'
    source += arrays('thai_special_rules', 'u8', special_data) + '\n'
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(source, encoding='utf-8')
    parts = dict(first=len(first), masks=len(masks64) * 8, bases=len(bases) * 2,
                 offsets=len(offsets) * 2, flags=len(packed_flags), fingerprints=len(payload),
                 special_starts=len(special_starts) * 2, special_rules=len(special_data))
    report = dict(status='verified_encoding', dll_sha256=DLL_SHA256, buckets=len(rows),
                  fingerprint_keys=keys, maximum_word_length=14, special_rules=len(rules),
                  payload_parts=parts, payload_bytes=sum(parts.values()),
                  reference_ordinary_bytes=original_bytes, reference_special_bytes=original_special,
                  source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                  generated_sha256=hashlib.sha256(args.output.read_bytes()).hexdigest(),
                  runtime_library_dependency=False,
                  provenance='Normalized lexical fingerprints and conditional prefix behavior from the pinned installed Windows reference; not the ICU/PyThaiNLP lexicon.')
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
