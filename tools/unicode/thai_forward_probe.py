"""Pinned observations and differential model of the Thai forward cursor.

The controller model uses private primitive calls in QA. Native replay uses
only the first-party lexical, selection and syllable implementations.
"""
import argparse
import ctypes
import hashlib
import json
from pathlib import Path
import struct
import time

from thai_selection_probe import Reference, inputs, engine_spans
from thai_unknown_probe import DLL_SHA256, UNKNOWN_RVA, PREFIX


def model(reference, unknown, odd):
    data = reference.buffer.raw[:reference.length]
    if len(data) <= 1:
        return ()
    cursor = current = 0
    unknown_fragment = False
    result = []
    slots = len(data)-1
    def byte(at):
        return data[at] if at < len(data) else 0
    def clear():
        struct.pack_into('<i', reference.state, 0x18, 0)
    def emit():
        nonlocal current, slots
        if not current:
            return False
        result.append(current)
        current = 0
        slots -= 1
        return slots != 0
    while cursor < len(data):
        here = byte(cursor)
        if here <= 0x9f:
            if current == 255:
                if not emit():
                    break
                current = 1
            else:
                current += 1
                clear()
            unknown_fragment = False
            cursor += 1
            continue
        if here == 0xcf:
            length = 3 if data[cursor:cursor+3] == b'\xcf\xc5\xcf' else 4 if data[cursor:cursor+4] == b'\xcf\xbe\xb3\xcf' else 1
            current = (current + length) & 255
            if not emit():
                break
            cursor += length
            continue
        if byte(cursor+1) <= 0xa0:
            current = (current+1) & 255
            if not emit():
                break
            cursor += 1
            continue
        output, retained, count = reference.selection(cursor)
        length = output[0]
        if count:
            if unknown_fragment:
                unknown_fragment = False
                if current >= 3 and not emit():
                    break
            if count > 1 and output[0]-output[1] < 3:
                cursor += length
                current = (current+length) & 255
                continue
        elif not length:
            length = unknown(reference.base+cursor, reference.base+len(data))-1
            if length <= 1:
                unknown_fragment = True
                length = 1 + (here > 0xce)
                cursor += length
                current = (current+length) & 255
                while cursor <= len(data) and not odd[byte(cursor)]:
                    cursor += 1
                    current = (current+1) & 255
                continue
        assert length > 0
        cursor += length
        current = (current+length) & 255
        while byte(cursor) in (0xe6, 0xcf):
            cursor += 1
            current = (current+1) & 255
            clear()
        if cursor > len(data):
            break
        if byte(cursor) > 0x9f and not emit():
            break
    if current:
        result.append(current)
    return tuple(result)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input',required=True,type=Path)
    parser.add_argument('--start',type=int,default=1)
    parser.add_argument('--count',type=int,default=4096)
    parser.add_argument('--output',required=True,type=Path)
    parser.add_argument('--report',required=True,type=Path)
    args=parser.parse_args()
    assert args.start>0 and 0<args.count<=4096
    assert not args.output.exists() and not args.report.exists()
    started=time.perf_counter();reference=Reference();base=reference.dll._handle
    assert ctypes.string_at(base+0x1d4d0c,11)==bytes.fromhex('40 55 53 56 57 41 54 41 56 41 57')
    assert ctypes.string_at(base+UNKNOWN_RVA,len(PREFIX))==PREFIX
    forward=ctypes.WINFUNCTYPE(None,ctypes.c_void_p,ctypes.c_int,ctypes.c_void_p,ctypes.c_int)(base+0x1d4d0c)
    unknown=ctypes.WINFUNCTYPE(ctypes.c_int,ctypes.c_void_p,ctypes.c_void_p)(base+UNKNOWN_RVA)
    odd=[v&1 for v in struct.unpack('<256i',ctypes.string_at(base+0x394b20,1024))]
    payload=bytearray(b'MCTHSEG1'+bytes(4));sources=spans=segments=0;differences=[]
    for identity,text in inputs(args.input):
        if identity<args.start:continue
        if identity>=args.start+args.count:break
        sources+=1
        for offset,span in engine_spans(text):
            reference.prepare(span)
            output=ctypes.create_string_buffer(len(span)+64)
            forward(reference.base,len(span),output,len(span))
            values=tuple(output.raw.split(b'\0',1)[0])
            expected=model(reference,unknown,odd)
            if values!=expected and len(differences)<20:
                differences.append(dict(identity=identity,text=span,reference=values,model=expected))
            payload+=struct.pack('<I',len(span))+struct.pack('<'+'I'*len(span),*map(ord,span))
            payload+=struct.pack('<I',len(values))+bytes(values)
            spans+=1;segments+=len(values)
    struct.pack_into('<I',payload,8,spans);args.output.write_bytes(payload)
    report=dict(status='different' if differences else 'passed',sources=sources,spans=spans,segments=segments,
                differences=differences,seconds=time.perf_counter()-started,start=args.start,count=args.count,
                input=dict(path=str(args.input.resolve()),sha256=hashlib.sha256(args.input.read_bytes()).hexdigest()),
                fixture_sha256=hashlib.sha256(payload).hexdigest(),source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                observer_sha256=hashlib.sha256(Path(__file__).with_name('thai_selection_probe.py').read_bytes()).hexdigest(),dll_sha256=DLL_SHA256)
    args.report.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8');print(json.dumps(report,ensure_ascii=False))
    return bool(differences)


if __name__=='__main__':
    raise SystemExit(main())
