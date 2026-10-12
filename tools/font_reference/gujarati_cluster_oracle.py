"""Bounded Gujarati scanner comparisons; final Windows clusters constrain parsing only."""
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


def corpus(stress=False,vowels=False):
    cases = {}
    def add(s, group):
        s = tuple(map(ord, s)) if isinstance(s, str) else tuple(s)
        if s:
            cases.setdefault(s, group)
    alphabet = (0xa95,0xab0,0xa85,0xabc,0xacd,0x200c,0x200d,0xabf,0xac1,0xabe,0xacb,0xacc,0xa81,0xa83)
    for scalar in range(0xa80,0xb00):
        for s in ((scalar,), (0xa95,scalar), (scalar,0xacd,0xa95), (0xa95,scalar,0xabf), (0xa95,scalar,scalar)):
            add(s,'scalar-coverage')
    for base in (0xa95,0xab0,0xa85,0xa0,0x25cc,0xabf,0x200d):
        for pair in itertools.product(alphabet,repeat=2):
            add((base,*pair),'pairs')
    for prefix in ((0xa95,0xacd),(0xa95,0xabf),(0xab0,0xacd),(0xa95,0xacd,0x200d),(0xa95,0xacd,0x200c)):
        for pair in itertools.product(alphabet,repeat=2):
            add((*prefix,*pair),'tails')
    for word in 'ગુજરાતી ભારત ભાષા નમસ્તે મિત્ર સમાચાર કમ્પ્યુટર અમદાવાદ પ્રાર્થના શ્રદ્ધા સૂર્ય દર્શન લક્ષ્મી ર્ક ર્કો ક્‌ષ ક્‍ષ ક્ષૌ'.split():
        add(word,'words')
        for end in range(1,len(word)):
            add(word[:end],'partial-input')
    for vowel in (0xa85,0xa86,0xa87,0xa88,0xa89,0xa8a,0xa8b,0xa8c,0xa8d,0xa8f,0xa90,0xa91,0xa93,0xa94,0xae0,0xae1):
        for mark in range(0xabe,0xace):
            add((vowel,mark),'independent-vowels')
    for sign in (0xac5,0xac9,0xafb,0xafc,0xafd,0xafe,0xaff):
        for other in alphabet+(0xac5,0xac9,0xafb,0xafc,0xafd,0xafe,0xaff):
            add((0xa95,sign,other),'sign-tails')
            add((0xa95,other,sign),'sign-tails')
    for link in ((0xacd,), (0xacd,0x200d),(0x200d,),(0xacd,0x200c)):
        add((0xa95,*link)*40+(0xabf,),'long')
    for sign in (0xabf,0xabe,0xacb,0xa81,0xa83,0x200c):
        add((0xa95,)+(sign,)*100,'long')
    if stress:
        cases={}
        rng=random.Random(20260923)
        for _ in range(1024):
            add(tuple(rng.choice(alphabet+(0xac0,0xac7,0xac8,0xaf0,0xace,0xad7,0xac5,0xac9,0xafb,0xafc,0xafd,0xafe,0xaff,0xa0,0x25cc)) for _ in range(rng.randint(1,40))),'holdout')
    if vowels:
        cases={}
        for vowel in (0xa85,0xa86,0xa8f,0xa93):
            for link in ((0x200d,),(0x200c,),(0xa0,),(0xacd,),(0x200d,0xacd),(0xacd,0x200d),(0xacd,0x200d,0xabc)):
                for base in (0xa95,0xab0,0x25cc):
                    for mark in range(0xabe,0xacd):
                        add((vowel,*link,base,mark),'vowel-join-holdout')
                        add((vowel,*link,base,0xa0,mark),'vowel-nbsp-holdout')
    return list(cases.items())


def verify(args):
    started=time.perf_counter()
    cases=[(tuple(s),'explicit') for s in json.loads(args.case_file.read_text(encoding='utf-8'))] if args.case_file else corpus(args.stress,args.vowels)
    if not cases or any(not s or len(s)>256 or any(type(c) is not int or not 0<=c<=65535 or 0xd800<=c<=0xdfff for c in s) for s,_ in cases):
        raise ValueError('Expected nonempty BMP input within observer capacity')
    inputs=[identity(args.font),identity(args.reference)]
    if args.observations:
        saved=json.loads(args.observations.read_text(encoding='utf-8'))
        if saved['inputs']!=inputs or [r['scalars'] for r in saved['rows']]!=[list(s) for s,_ in cases]:
            raise ValueError('Stale font, observer or corpus observations')
        rows=saved['rows']
    else:
        rows=observe(args,cases,seed='a95')
    (args.output_dir/'observations.json').write_text(json.dumps(dict(inputs=inputs,rows=rows)),encoding='utf-8')
    payload=b'MDVCL1\0\0'+struct.pack('<I',len(cases))
    for s,_ in cases:
        payload+=struct.pack('<I',len(s))+struct.pack('<'+'I'*len(s),*s)
    native=subprocess.run([str(args.native),'--gujarati-clusters'],input=payload,capture_output=True,timeout=2,creationflags=subprocess.CREATE_NO_WINDOW)
    if native.returncode or native.stdout[:12]!=payload[:12] or len(native.stdout)!=12+sum(len(s) for s,_ in cases):
        raise RuntimeError('Invalid native cluster response: '+repr(native.stdout[-200:]))
    cursor=12; mismatches=0; unavailable=0
    for row in rows:
        count=len(row['scalars']);row['native']=list(native.stdout[cursor:cursor+count]);cursor+=count
        row['exact']=None if 'unavailable' in row else row['native']==row['expected']
        mismatches+=row['exact'] is False;unavailable+=row['exact'] is None
    result=dict(status='failed' if mismatches else 'incomplete' if unavailable else 'passed',cases=len(rows),
        mismatches=mismatches,unavailable=unavailable,seconds=time.perf_counter()-started,inputs=inputs,native=identity(args.native),rows=rows,
        scope='Gujarati input scanner versus final Windows source-group boundaries; no glyph, positioning or app parity established.')
    (args.output_dir/'result.json').write_text(json.dumps(result),encoding='utf-8')
    print(f"{len(rows)-mismatches-unavailable}/{len(rows)} exact boundary vectors, {mismatches} differences, {unavailable} unavailable in {result['seconds']:.2f}s")
    return 1 if mismatches else 2 if unavailable else 0


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for key in ('font','native','reference','output-dir'):
        parser.add_argument('--'+key,type=Path,required=True)
    parser.add_argument('--observations',type=Path)
    corpus_options=parser.add_mutually_exclusive_group()
    corpus_options.add_argument('--stress',action='store_true')
    corpus_options.add_argument('--vowels',action='store_true')
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
    if args.stress:
        command.append('--stress')
    if args.vowels:
        command.append('--vowels')
    if args.case_file:
        command.extend(['--case-file',str(args.case_file.resolve())])
    try:
        row=run_check('gujarati-clusters',command,Path.cwd(),args.output_dir,5,threading.Event())
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
