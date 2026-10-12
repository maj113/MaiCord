"""Bounded Tamil shaping-cluster observations; no production dependencies.

The frozen Windows observer is seeded with Tamil, not Devanagari. Final glyph
cluster maps constrain the parser, but do not validate glyphs or reordering.
Use --observations to reuse an unchanged reference run while editing the parser.
"""
import argparse
import hashlib
import itertools
import json
import os
from pathlib import Path
import struct
import subprocess
import time


def corpus():
    cases = {}
    def add(values, group):
        values = tuple(map(ord, values)) if isinstance(values, str) else tuple(values)
        if values:
            cases.setdefault(values, group)
    for scalar in range(0xb80, 0xc00):
        for values in ((scalar,), (0xb95, scalar), (scalar, 0xbcd, 0xb95),
                       (0xb95, scalar, 0xbc6), (0xb95, scalar, scalar)):
            add(values, 'scalar-coverage')
    alphabet = (0xb95, 0xbb0, 0xb85, 0xbcd, 0x200c, 0x200d, 0xbc6,
                0xbc0, 0xbbf, 0xbbe, 0xbc1, 0xbca, 0xb82, 0xbd7)
    for base in (0xb95, 0xbb0, 0xb85, 0xa0, 0x25cc, 0xbc6, 0x200d):
        for pair in itertools.product(alphabet, repeat=2):
            add((base, *pair), 'pairs')
    for prefix in ((0xb95, 0xbcd), (0xb95, 0xbc6), (0xbb0, 0xbcd),
                   (0xb95, 0xbcd, 0x200d), (0xb95, 0xbcd, 0x200c)):
        for pair in itertools.product(alphabet, repeat=2):
            add((*prefix, *pair), 'tails')
    for word in ('தமிழ் வணக்கம் நண்பர்கள் கணினி செய்திகள் கோவில் கௌரவம் '
                 'க்ஷேத்திரம் ஸ்ரீ நூலகம் சென்னை பொறியியல் கெள கொ கோ').split():
        add(word, 'words')
        for end in range(1, len(word)):
            add(word[:end], 'partial-input')
    for link in ((0xbcd,), (0xbcd, 0x200d), (0x200d,), (0xbcd, 0x200c)):
        add((0xb95, *link)*40+(0xbc6,), 'long')
    for sign in (0xbc6, 0xbbe, 0xbd7, 0xb82, 0x200c):
        add((0xb95,)+(sign,)*100, 'long')
    return [(s, g) for s, g in cases.items()]


def identity(path):
    return dict(path=str(path), sha256=hashlib.sha256(path.read_bytes()).hexdigest())


def observe(args, cases, *, seed='b95'):
    rows=[]; deadline=time.monotonic()+5
    while len(rows)<len(cases):
        remaining=cases[len(rows):]
        payload=bytearray(b'MCSHPB1\0'+struct.pack('<I',len(remaining)))
        for scalars,_ in remaining:
            payload+=struct.pack('<II',len(scalars),0)+struct.pack('<'+'H'*len(scalars),*scalars)
        budget=min(2,deadline-time.monotonic())
        if budget<=0:
            raise TimeoutError('Reference observation deadline')
        proc=subprocess.run([str(args.reference),str(args.font),seed,'default','1148846080','0','0','en-US'],
            input=payload,capture_output=True,timeout=budget,env=dict(os.environ,MAICORD_SHAPE_BATCH='1'),creationflags=subprocess.CREATE_NO_WINDOW)
        if proc.stdout[:12]!=payload[:12]:
            raise RuntimeError('Invalid reference header')
        cursor=12
        for scalars,group in remaining:
            if cursor==len(proc.stdout):
                break
            count=struct.unpack_from('<I',proc.stdout,cursor)[0];cursor+=4
            clusters=struct.unpack_from('<'+'H'*len(scalars),proc.stdout,cursor+count*2)
            cursor+=count*14+len(scalars)*2
            if cursor>len(proc.stdout):
                raise RuntimeError('Truncated reference record')
            rows.append(dict(scalars=list(scalars),group=group,expected=[int(not i or clusters[i]!=clusters[i-1]) for i in range(len(scalars))]))
        if cursor!=len(proc.stdout):
            raise RuntimeError('Trailing reference output')
        if not proc.returncode:
            if len(rows)!=len(cases):
                raise RuntimeError('Missing successful reference records')
            break
        if len(rows)==len(cases):
            raise RuntimeError('Reference failed after final record')
        scalars,group=cases[len(rows)]
        budget=min(2,deadline-time.monotonic())
        if budget<=0:
            raise TimeoutError('Reference observation deadline')
        single=subprocess.run([str(args.reference),str(args.font),','.join(f'{c:x}' for c in scalars),
            'default','1148846080','0','0','en-US'],capture_output=True,timeout=budget,creationflags=subprocess.CREATE_NO_WINDOW)
        failure=json.loads(single.stdout)
        if not (failure['hresult'] or failure['placement_hresult']):
            raise RuntimeError('Batch failure was not reproduced')
        rows.append(dict(scalars=list(scalars),group=group,unavailable=failure))
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--font', type=Path, required=True)
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--observations', type=Path)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--group', action='append')
    parser.add_argument('--case', action='append', help='Comma-separated hexadecimal scalars')
    args = parser.parse_args()
    start = time.perf_counter()
    args.font = args.font.resolve(); args.reference = args.reference.resolve()
    cases = [(tuple(int(c, 16) for c in s.split(',')), 'explicit') for s in args.case] if args.case else corpus()
    if args.group:
        cases = [(s,g) for s,g in cases if g in args.group]
    if not cases or any(not s or len(s)>256 or any(c>65535 or 0xd800<=c<=0xdfff for c in s) for s,_ in cases):
        raise ValueError('Expected nonempty BMP cases within the observer capacity')
    inputs = [identity(args.font), identity(args.reference)]
    args.output_dir.mkdir(parents=True, exist_ok=True)
    if args.observations:
        saved = json.loads(args.observations.read_text(encoding='utf-8'))
        if [v['sha256'] for v in saved['inputs']] != [v['sha256'] for v in inputs] or [r['scalars'] for r in saved['rows']] != [list(s) for s,_ in cases]:
            raise ValueError('Stale observations: font, observer or corpus changed')
        rows = saved['rows']
    else:
        rows=observe(args,cases)
    (args.output_dir/'observations.json').write_text(json.dumps(dict(inputs=inputs,rows=rows))+'\n',encoding='utf-8')
    result=dict(status='observed',cases=len(cases),reference_unavailable=sum('unavailable' in row for row in rows),seconds=time.perf_counter()-start,
        scope='Windows Tamil-context final source clusters; not scanner boundaries or native parity')
    (args.output_dir/'result.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({k:v for k,v in result.items() if k!='differences'}))
    return 0


if __name__=='__main__':
    raise SystemExit(main())
