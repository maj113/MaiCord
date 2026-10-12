"""QA-only, version-pinned observations of Windows Thai unknown-word recovery.

The private entry point was identified using public symbols and independently
disassembled with LLVM. Never import this observer from the runtime. A different
DLL is rejected before resolving or calling its private address.
"""
import argparse
import ctypes
import hashlib
import itertools
import json
import os
from pathlib import Path
import struct
import time


DLL_SHA256 = '4a83f1e02c5558c50ab9c0e94b095a14b4188962a64abd495a79b607d50773ef'
UNKNOWN_RVA = 0x1d5724
PREFIX = bytes.fromhex('48 89 5c 24 10 57 48 81 ec a0 02 00 00')


def cancellation_span(text):
    """Observed fast recovery branch; None delegates to the syllable parser.

    Return consumed characters (the Windows helper returns this plus one).
    This branch is not a complete word breaker or an unknown-state detector.
    """
    if len(text) <= 13:
        return None
    for index in range(10, 0, -1):
        if text[index] == '\u0e4c':
            end = index + 1
            if ('\u0e01' <= text[end] <= '\u0e2e'
                    and '\u0e40' <= text[end + 1] <= '\u0e44'):
                end += 1
            return end
    return None


def cases():
    # Full Cartesian neighbor coverage, including all consonants and leading
    # vowels. Each case reaches the fast branch, so it has an exact prediction.
    for length, index, consonant, following in itertools.product(
            (14, 15, 24, 64), range(1, 11),
            range(0xe00, 0xe30), range(0xe3f, 0xe46)):
        chars = ['ก'] * length
        chars[index] = '์'
        chars[index + 1] = chr(consonant)
        chars[index + 2] = chr(following)
        yield ''.join(chars)
    # Search order and transparent spaces/ZWSP are separate from syllable
    # grammar. The adapter converts ZWSP to a space before the byte engine.
    for first, second, separator in itertools.product(range(1, 11), range(1, 11), ('ก', ' ', '\u200b')):
        chars = [separator] * 24
        chars[first] = chars[second] = '์'
        yield ''.join(chars)
    # These do not reach the fast branch; retain their parser observations,
    # without pretending that the partial predictor explains them.
    for length, index in itertools.product((11, 12, 13, 14, 15), range(13)):
        if index < length:
            chars = ['ก'] * length
            chars[index] = '์'
            yield ''.join(chars)


def encode(text):
    result = bytearray()
    for char in text:
        cp = ord(char)
        if 0xe01 <= cp <= 0xe5b:
            result.append(cp - 0xe00 + 0xa0)
        elif char in (' ', '\u200b'):
            result.append(0x20)
        elif cp == 0xe00:
            # Synthetic byte-neighbor probe: below the Thai consonant range.
            result.append(0xa0)
        else:
            raise ValueError(f'Unmapped probe scalar U+{cp:04X}')
    return bytes(result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', required=True, type=Path)
    args = parser.parse_args()
    started = time.perf_counter()
    out = args.output_dir
    out.mkdir(parents=True, exist_ok=True)
    target = out / 'unknown.json'
    raw = out / 'unknown.bin'
    if target.exists() or raw.exists():
        raise ValueError('Evidence already exists; choose a new observation directory')
    dll_path = Path(os.environ['SystemRoot']) / 'System32/DWrite.dll'
    dll_bytes = dll_path.read_bytes()
    dll_hash = hashlib.sha256(dll_bytes).hexdigest()
    if ctypes.sizeof(ctypes.c_void_p) != 8 or dll_hash != DLL_SHA256:
        raise RuntimeError('Unsupported Windows DLL or process architecture')
    dll = ctypes.WinDLL(str(dll_path))
    address = dll._handle + UNKNOWN_RVA
    if ctypes.string_at(address, len(PREFIX)) != PREFIX:
        raise RuntimeError('Private entry point bytes differ')
    unknown = ctypes.WINFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p)(address)
    count = predicted = exact = 0
    mismatches = []
    observations = bytearray(b'MCTHUNK1')
    fallthrough = []
    for text in cases():
        data = encode(text)
        # Terminated and padded for the independently observed parser lookahead.
        buffer = ctypes.create_string_buffer(data, len(data) + 64)
        base = ctypes.addressof(buffer)
        value = unknown(base, base + len(data))
        expected = cancellation_span(text)
        observations += struct.pack('<Ii', len(data), value) + data
        count += 1
        if expected is not None:
            predicted += 1
            if value == expected + 1:
                exact += 1
            else:
                mismatches.append(dict(text=text, expected=expected + 1, observed=value))
        else:
            fallthrough.append(dict(text=text, observed=value))
    raw.write_bytes(observations)
    report = dict(status='passed' if exact == predicted else 'different',
                  cases=count, predicted=predicted, exact=exact,
                  fallthrough=fallthrough, mismatches=mismatches,
                  seconds=time.perf_counter() - started, runtime_modified=False,
                  scope='Private recovery fast-branch contract only; no complete segmentation acceptance.',
                  dll=dict(path=str(dll_path), sha256=dll_hash, bytes=len(dll_bytes)),
                  entry_rva=UNKNOWN_RVA, entry_bytes=PREFIX.hex(),
                  source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                  observations=dict(path=str(raw.resolve()), bytes=len(observations),
                                    sha256=hashlib.sha256(observations).hexdigest()))
    target.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({k: report[k] for k in ('status', 'cases', 'predicted', 'exact', 'seconds')}))
    return 0 if report['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
