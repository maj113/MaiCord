"""Bounded native Myanmar scalar-run glyph/source/exact-placement comparison of cluster or run cases.

Unicode scalar cases within the observer capacity; optional retained UTF-8 paths.
Compact publication rejects final glyph zero by its existing fallback contract;
those rejections are counted separately, never as exact output comparisons.
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
    if not cases or any(not s or sum(1+(c>65535) for c in s)>256 or any(type(c) is not int or not 0<=c<=0x10ffff or 0xd800<=c<=0xdfff for c in s) for s,_ in cases):
        raise ValueError('Expected valid Unicode input within the 256-UTF16-unit observer limit')
    inputs=dict(font=identity(args.font),reference=identity(args.reference),cases=identity(args.case_file),
                independent=args.independent,rtl=args.rtl)
    if args.observations:
        saved=json.loads(args.observations.read_text(encoding='utf-8'))
        if saved['inputs']!=inputs:raise ValueError('Reference inputs changed')
        expected=saved['rows']
    else:
        expected=batch_glyphs(args.reference,args.font,[s for s,_ in cases],seed='1000',detailed=True,
            independent=args.independent,rtl=args.rtl,timeout=2)
    if len(expected)!=len(cases):raise ValueError('Incomplete reference observations')
    if not args.observations:(args.output_dir/'observations.json').write_text(json.dumps(dict(inputs=inputs,rows=expected)),encoding='utf-8')
    payload=bytearray(b'MDSHP1\0\0'+struct.pack('<I',len(cases)))
    for s,_ in cases:payload+=struct.pack('<I',len(s))+struct.pack('<'+'I'*len(s),*s)
    native=subprocess.run([str(args.native)],input=payload,capture_output=True,timeout=2,
        env=dict(os.environ,MAICORD_MYANMAR_FONT=str(args.font),MAICORD_MYANMAR_INDEPENDENT=str(int(args.independent)),
                 MAICORD_MYANMAR_RTL=str(int(args.rtl)),MAICORD_MYANMAR_OWNED=str(int(args.owned)),MAICORD_MYANMAR_COMPACT=str(int(args.compact))),creationflags=subprocess.CREATE_NO_WINDOW)
    if native.returncode:raise RuntimeError(repr(native.stdout[-500:])+repr(native.stderr))
    actual=decode(native.stdout,cases,True)
    rows=[];exact_count=0;missing_rejections=0
    for (s,_),a,b in zip(cases,actual,expected):
        if any(c>65535 for c in s):
            at=0;groups=[]
            for c in s:
                groups.append(b['clusters'][at])
                if c>65535 and b['clusters'][at+1]!=groups[-1]:raise RuntimeError('Reference surrogate source split')
                at+=1+(c>65535)
            b=dict(b,clusters=groups)
        exact=a['status']==0 and all(a[k]==b[k] for k in ('glyphs','clusters','placements'));exact_count+=exact
        missing=args.compact and 0 in b['glyphs'] and a['status']==8 and not a['glyphs'] and not a['placements'];missing_rejections+=missing
        if not args.failures_only or not exact:rows.append(dict(scalars=s,native=a,reference=b,exact=exact,expected_missing_rejection=missing))
    result=dict(status='passed' if exact_count+missing_rejections==len(cases) else 'failed',cases=len(cases),exact=exact_count,missing_rejections=missing_rejections,failures_only=args.failures_only,
        seconds=time.perf_counter()-started,inputs=inputs,observations=identity(args.observations) if args.observations else None,native=identity(args.native),scope='Myanmar owned UTF-8 compact' if args.compact else 'Myanmar owned UTF-8 rich' if args.owned else 'Myanmar full scalar run' if args.run else 'Myanmar single cluster',rows=rows)
    (args.output_dir/'result.json').write_text(json.dumps(result),encoding='utf-8')
    print(f'{result["exact"]}/{len(cases)} exact shapes, {missing_rejections} expected missing-glyph rejections in {result["seconds"]:.2f}s')
    return 0 if result['status']=='passed' else 1


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for key in ('font','reference','native','case-file','output-dir'):parser.add_argument('--'+key,type=Path,required=True)
    parser.add_argument('--observations',type=Path)
    parser.add_argument('--independent',action='store_true')
    parser.add_argument('--rtl',action='store_true')
    parser.add_argument('--run',action='store_true')
    parser.add_argument('--owned',action='store_true')
    parser.add_argument('--compact',action='store_true')
    parser.add_argument('--failures-only',action='store_true',help='Retain full details only for mismatches; saved Windows observations remain complete')
    parser.add_argument('--worker',action='store_true',help=argparse.SUPPRESS)
    args=parser.parse_args()
    for key in ('font','reference','native','case_file','output_dir'):setattr(args,key,getattr(args,key).resolve())
    if args.worker:return worker(args)
    if args.output_dir.exists() and any(args.output_dir.iterdir()):parser.error('Preserve existing evidence; choose a new output directory')
    args.output_dir.mkdir(parents=True,exist_ok=True)
    if args.observations and args.observations.resolve()==(args.output_dir/'observations.json').resolve():parser.error('Keep saved observations separate')
    path=args.output_dir/'result.json';path.write_text('{"status":"running"}',encoding='utf-8')
    sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
    from verify_native_text import run_check,CheckFailure
    try:execution=run_check('myanmar-shape',[sys.executable,str(Path(__file__).resolve()),*sys.argv[1:],'--worker'],Path.cwd(),args.output_dir,5,threading.Event())
    except CheckFailure as error:execution=error.row
    result=json.loads(path.read_text(encoding='utf-8'));result['execution']=execution
    if execution['exit']:result['status']=execution['status']
    path.write_text(json.dumps(result),encoding='utf-8')
    print({k:result[k] for k in ('status','cases','exact') if k in result})
    return execution['exit']


if __name__=='__main__':raise SystemExit(main())
