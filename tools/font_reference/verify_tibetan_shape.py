"""Bounded Tibetan glyph/source/exact-placement comparisons.

Checks one-cluster cases, complete scalar runs with --run, or retained UTF-8
font admission with --owned/--compact. Final-zero compact rejections are counted
separately from exact output. App pixels and universal font/axis coverage are
not established. UTF-16 sources are projected to input scalars.
"""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import threading
import time
from devanagari_plan_oracle import batch_glyphs
from tamil_shape_oracle import decode
from tamil_cluster_oracle import identity


def worker(args):
    started=time.perf_counter()
    cases=[(tuple(s),'explicit') for s in json.loads(args.case_file.read_text(encoding='utf-8'))]
    limit=256 if args.run or args.owned or args.compact else 62
    if not cases or any(not s or len(s)>limit or any(type(c) is not int or not 0<=c<=0x10ffff or 0xd800<=c<=0xdfff for c in s) or sum(1+(c>65535) for c in s)>256 for s,_ in cases):
        raise ValueError('Expected nonempty scalar input within the mode and reference UTF-16 limits')
    inputs=dict(font=identity(args.font),reference=identity(args.reference),cases=identity(args.case_file),
                independent=args.independent,rtl=args.rtl,run=args.run)
    if args.observations:
        saved=json.loads(args.observations.read_text(encoding='utf-8'))
        if dict(saved['inputs'],run=saved['inputs'].get('run',False))!=inputs:raise ValueError('Reference inputs changed')
        expected=saved['rows']
    else:
        expected=batch_glyphs(args.reference,args.font,[s for s,_ in cases],seed='0f40',detailed=True,
            independent=args.independent,rtl=args.rtl,timeout=2)
    if len(expected)!=len(cases):raise ValueError('Incomplete reference observations')
    (args.output_dir/'observations.json').write_text(json.dumps(dict(inputs=inputs,rows=expected)),encoding='utf-8')
    payload=bytearray(b'MDSHP1\0\0'+struct.pack('<I',len(cases)))
    for s,_ in cases:payload+=struct.pack('<I',len(s))+struct.pack('<'+'I'*len(s),*s)
    native=subprocess.run([str(args.native)],input=payload,capture_output=True,timeout=2,
        env=dict(os.environ,MAICORD_TIBETAN_FONT=str(args.font),MAICORD_TIBETAN_INDEPENDENT=str(int(args.independent)),
                 MAICORD_TIBETAN_RTL=str(int(args.rtl)),
                 MAICORD_TIBETAN_OWNED=str(int(args.owned or args.compact)),MAICORD_TIBETAN_COMPACT=str(int(args.compact))),creationflags=subprocess.CREATE_NO_WINDOW)
    if native.returncode:raise RuntimeError(repr(native.stdout[-500:])+repr(native.stderr))
    actual=decode(native.stdout,cases,True)
    rows=[]
    for (s,_),a,b in zip(cases,actual,expected):
        source=0;clusters=[]
        for scalar in s:
            clusters.append(b['clusters'][source])
            if scalar>65535 and b['clusters'][source+1]!=clusters[-1]:raise ValueError('Split surrogate source')
            source+=1+(scalar>65535)
        b=dict(b,clusters=clusters)
        missing=args.compact and 0 in b['glyphs']
        rejected=missing and a['status']==8 and not a['glyphs'] and not a['placements']
        rows.append(dict(scalars=s,native=a,reference=b,missing_rejection=rejected,exact=not missing and a['status']==0 and all(a[k]==b[k] for k in ('glyphs','clusters','placements'))))
    result=dict(status='passed' if all(r['exact'] or r['missing_rejection'] for r in rows) else 'failed',cases=len(rows),exact=sum(r['exact'] for r in rows),missing_rejections=sum(r['missing_rejection'] for r in rows),
        seconds=time.perf_counter()-started,inputs=inputs,native=identity(args.native),owned=args.owned or args.compact,compact=args.compact,scope=__doc__,rows=rows)
    (args.output_dir/'result.json').write_text(json.dumps(result),encoding='utf-8')
    print(f'{result["exact"]}/{len(rows)} exact shapes in {result["seconds"]:.2f}s')
    return 0 if result['status']=='passed' else 1


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for key in ('font','reference','native','case-file','output-dir'):parser.add_argument('--'+key,type=Path,required=True)
    parser.add_argument('--observations',type=Path)
    parser.add_argument('--independent',action='store_true')
    parser.add_argument('--rtl',action='store_true')
    parser.add_argument('--run',action='store_true',help='Shape complete scalar runs')
    parser.add_argument('--owned',action='store_true',help='Use retained font admission and UTF-8 adapters')
    parser.add_argument('--compact',action='store_true',help='Use retained compact publication')
    parser.add_argument('--worker',action='store_true',help=argparse.SUPPRESS)
    args=parser.parse_args()
    for key in ('font','reference','native','case_file','output_dir'):setattr(args,key,getattr(args,key).resolve())
    if args.worker:return worker(args)
    args.output_dir.mkdir(parents=True,exist_ok=True)
    if args.observations and args.observations.resolve()==(args.output_dir/'observations.json').resolve():parser.error('Keep saved observations separate')
    path=args.output_dir/'result.json'
    if path.exists():parser.error('Preserve existing evidence; select an unused output directory')
    path.write_text('{"status":"running"}',encoding='utf-8')
    sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
    from verify_native_text import run_check,CheckFailure
    try:execution=run_check('tibetan-shape',[sys.executable,str(Path(__file__).resolve()),*sys.argv[1:],'--worker'],Path.cwd(),args.output_dir,5,threading.Event())
    except CheckFailure as error:execution=error.row
    result=json.loads(path.read_text(encoding='utf-8'));result['execution']=execution
    if execution['exit']:result['status']=execution['status']
    path.write_text(json.dumps(result),encoding='utf-8')
    print({k:result[k] for k in ('status','cases','exact') if k in result})
    return execution['exit']


if __name__=='__main__':raise SystemExit(main())
