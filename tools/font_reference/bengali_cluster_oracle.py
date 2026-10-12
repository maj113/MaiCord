"""Bounded Bengali scanner comparisons; final Windows clusters constrain parsing only."""
import argparse
import itertools
import json
import os
from pathlib import Path
import random
import struct
import subprocess
import sys
import threading
import time
from tamil_cluster_oracle import identity, observe


def corpus(stress=False):
    cases = {}
    def add(s, group):
        s = tuple(map(ord, s)) if isinstance(s, str) else tuple(s)
        if s:
            cases.setdefault(s, group)
    alphabet = (0x995,0x9b0,0x985,0x9bc,0x9cd,0x200c,0x200d,0x9bf,0x9c1,0x9be,0x9cb,0x9cc,0x981,0x983)
    for scalar in range(0x980,0xa00):
        for s in ((scalar,), (0x995,scalar), (scalar,0x9cd,0x995), (0x995,scalar,0x9bf), (0x995,scalar,scalar)):
            add(s,'scalar-coverage')
    for base in (0x995,0x9b0,0x985,0xa0,0x25cc,0x9bf,0x200d):
        for pair in itertools.product(alphabet,repeat=2):
            add((base,*pair),'pairs')
    for prefix in ((0x995,0x9cd),(0x995,0x9bf),(0x9b0,0x9cd),(0x995,0x9cd,0x200d),(0x995,0x9cd,0x200c)):
        for pair in itertools.product(alphabet,repeat=2):
            add((*prefix,*pair),'tails')
    for word in 'বাংলা বাংলাদেশ বন্ধু সংবাদ কম্পিউটার কলকাতা কৌতূহল প্রার্থনা শ্রদ্ধা সূর্য দর্শন লক্ষ্মী র্ক র্কো ক্‌ষ ক্‍ষ ক্ষৌ অসমীয়া ৰাষ্ট্ৰ'.split():
        add(word,'words')
        for end in range(1,len(word)):
            add(word[:end],'partial-input')
    for link in ((0x9cd,), (0x9cd,0x200d),(0x200d,),(0x9cd,0x200c)):
        add((0x995,*link)*40+(0x9bf,),'long')
    for sign in (0x9bf,0x9be,0x9cb,0x981,0x983,0x200c):
        add((0x995,)+(sign,)*100,'long')
    if stress:
        cases={}
        rng=random.Random(20260922)
        for _ in range(1024):
            add(tuple(rng.choice(alphabet+(0x9c0,0x9c7,0x9c8,0x9f0,0x9ce,0x9d7,0xa0,0x25cc)) for _ in range(rng.randint(1,40))),'holdout')
    return list(cases.items())


def verify(args):
    started=time.perf_counter()
    cases=corpus(args.stress)
    inputs=[identity(args.font),identity(args.reference)]
    if args.observations:
        saved=json.loads(args.observations.read_text(encoding='utf-8'))
        if saved['inputs']!=inputs or [r['scalars'] for r in saved['rows']]!=[list(s) for s,_ in cases]:
            raise ValueError('Stale font, observer or corpus observations')
        rows=saved['rows']
    else:
        rows=observe(args,cases,seed='995')
    (args.output_dir/'observations.json').write_text(json.dumps(dict(inputs=inputs,rows=rows)),encoding='utf-8')
    payload=b'MDVCL1\0\0'+struct.pack('<I',len(cases))
    for s,_ in cases:
        payload+=struct.pack('<I',len(s))+struct.pack('<'+'I'*len(s),*s)
    native=subprocess.run([str(args.native),'--bengali-clusters'],input=payload,capture_output=True,timeout=2,creationflags=subprocess.CREATE_NO_WINDOW)
    if native.returncode or native.stdout[:12]!=payload[:12] or len(native.stdout)!=12+sum(len(s) for s,_ in cases):
        raise RuntimeError('Invalid native cluster response: '+repr(native.stdout[-200:]))
    cursor=12; mismatches=0; unavailable=0
    for row in rows:
        count=len(row['scalars']);row['native']=list(native.stdout[cursor:cursor+count]);cursor+=count
        row['exact']=None if 'unavailable' in row else row['native']==row['expected']
        mismatches+=row['exact'] is False;unavailable+=row['exact'] is None
    result=dict(status='failed' if mismatches else 'incomplete' if unavailable else 'passed',cases=len(rows),
        mismatches=mismatches,unavailable=unavailable,seconds=time.perf_counter()-started,inputs=inputs,native=identity(args.native),rows=rows,
        scope='Bengali input scanner versus final Windows source-group boundaries; no glyph, positioning or app parity established.')
    (args.output_dir/'result.json').write_text(json.dumps(result),encoding='utf-8')
    print(f"{len(rows)-mismatches-unavailable}/{len(rows)} exact boundary vectors, {mismatches} differences, {unavailable} unavailable in {result['seconds']:.2f}s")
    return 1 if mismatches else 2 if unavailable else 0


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for key in ('font','native','reference','output-dir'):
        parser.add_argument('--'+key,type=Path,required=True)
    parser.add_argument('--observations',type=Path)
    parser.add_argument('--stress',action='store_true')
    parser.add_argument('--worker',action='store_true',help=argparse.SUPPRESS)
    args=parser.parse_args()
    for key in ('font','native','reference','output_dir'):
        setattr(args,key,getattr(args,key).resolve())
    if args.worker:
        return verify(args)
    args.output_dir.mkdir(parents=True,exist_ok=True)
    if args.observations and args.observations.resolve()==(args.output_dir/'observations.json').resolve():
        parser.error('Keep cached observations separate from current output')
    result_path=args.output_dir/'result.json';result_path.write_text('{"status":"running"}',encoding='utf-8')
    sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
    from verify_native_text import CheckFailure,run_check
    command=[sys.executable,str(Path(__file__).resolve()),'--worker']
    for key in ('font','native','reference','output_dir'):
        command.extend(['--'+key.replace('_','-'),str(getattr(args,key))])
    if args.observations:
        command.extend(['--observations',str(args.observations.resolve())])
    if args.stress:
        command.append('--stress')
    try:
        row=run_check('bengali-clusters',command,Path.cwd(),args.output_dir,5,threading.Event())
    except CheckFailure as error:
        row=error.row
    result=json.loads(result_path.read_text(encoding='utf-8'));result['execution']=row
    if row['exit'] and result.get('status')=='running':
        result['status']=row['status']
    result_path.write_text(json.dumps(result),encoding='utf-8')
    print({k:v for k,v in result.items() if k in ('status','cases','mismatches','unavailable','seconds')})
    return row['exit']


if __name__=='__main__':
    raise SystemExit(main())
