"""Bounded Malayalam scanner comparisons; final Windows clusters constrain parsing only."""
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


from malayalam_cluster_samples import corpus


def verify(args):
    started=time.perf_counter()
    cases=[(tuple(s),'explicit') for s in json.loads(args.case_file.read_text(encoding='utf-8'))] if args.case_file else corpus()
    if not cases or any(not s or len(s)>256 or any(type(c) is not int or not 0<=c<=65535 or 0xd800<=c<=0xdfff for c in s) for s,_ in cases):
        raise ValueError('Expected nonempty BMP input within observer capacity')
    inputs=[identity(args.font),identity(args.reference)]
    if args.observations:
        saved=json.loads(args.observations.read_text(encoding='utf-8'))
        if saved['inputs']!=inputs or [r['scalars'] for r in saved['rows']]!=[list(s) for s,_ in cases]:
            raise ValueError('Stale font, observer or corpus observations')
        rows=saved['rows']
    else:
        rows=observe(args,cases,seed='d15')
    (args.output_dir/'observations.json').write_text(json.dumps(dict(inputs=inputs,rows=rows)),encoding='utf-8')
    payload=bytearray(b'MDVCL1\0\0'+struct.pack('<I',len(cases)))
    for s,_ in cases:
        payload+=struct.pack('<I',len(s))+struct.pack('<'+'I'*len(s),*s)
    native=subprocess.run([str(args.native),'--malayalam-clusters'],input=payload,capture_output=True,timeout=2,env=dict(os.environ,MAICORD_MALAYALAM_CLUSTER_STREAM='1',MAICORD_MALAYALAM_FONT=str(args.font)),creationflags=subprocess.CREATE_NO_WINDOW)
    if native.returncode or native.stdout[:12]!=payload[:12] or len(native.stdout)!=12+sum(len(s) for s,_ in cases):
        raise RuntimeError('Invalid native cluster response: '+repr(native.stdout[-200:]))
    cursor=12; mismatches=0; unavailable=0
    for row in rows:
        count=len(row['scalars']);row['native']=list(native.stdout[cursor:cursor+count]);cursor+=count
        row['exact']=None if 'unavailable' in row else row['native']==row['expected']
        mismatches+=row['exact'] is False;unavailable+=row['exact'] is None
    result=dict(status='failed' if mismatches else 'incomplete' if unavailable else 'passed',cases=len(rows),
        mismatches=mismatches,unavailable=unavailable,seconds=time.perf_counter()-started,inputs=inputs,native=identity(args.native),rows=rows,
        scope='Malayalam input scanner versus final Windows source-group boundaries; no glyph, positioning or app parity established.')
    (args.output_dir/'result.json').write_text(json.dumps(result),encoding='utf-8')
    print(f"{len(rows)-mismatches-unavailable}/{len(rows)} exact boundary vectors, {mismatches} differences, {unavailable} unavailable in {result['seconds']:.2f}s")
    return 1 if mismatches else 2 if unavailable else 0


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for key in ('font','native','reference','output-dir'):
        parser.add_argument('--'+key,type=Path,required=True)
    parser.add_argument('--observations',type=Path)
    corpus_options=parser.add_mutually_exclusive_group()
    corpus_options.add_argument('--case-file',type=Path)
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
    if args.case_file:
        command.extend(['--case-file',str(args.case_file.resolve())])
    try:
        row=run_check('malayalam-clusters',command,Path.cwd(),args.output_dir,5,threading.Event())
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
