"""Bounded Telugu scanner comparisons; final Windows clusters constrain parsing only."""
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
    alphabet = (0xc15,0xc30,0xc05,0xc3c,0xc4d,0x200c,0x200d,0xc3f,0xc41,0xc3e,0xc4b,0xc4c,0xc01,0xc03)
    for scalar in range(0xc00,0xc80):
        for s in ((scalar,), (0xc15,scalar), (scalar,0xc4d,0xc15), (0xc15,scalar,0xc3f), (0xc15,scalar,scalar)):
            add(s,'scalar-coverage')
    for base in (0xc15,0xc30,0xc05,0xa0,0x25cc,0xc3f,0x200d):
        for pair in itertools.product(alphabet,repeat=2):
            add((base,*pair),'pairs')
    for prefix in ((0xc15,0xc4d),(0xc15,0xc3f),(0xc30,0xc4d),(0xc15,0xc4d,0x200d),(0xc15,0xc4d,0x200c)):
        for pair in itertools.product(alphabet,repeat=2):
            add((*prefix,*pair),'tails')
    for word in 'తెలుగు భాష భారత నమస్కారం మిత్ర వార్తలు కంప్యూటర్ హైదరాబాద్ ప్రార్థన శ్రద్ధ సూర్య లక్ష్మీ క్ష జ్ఞ ర్క కై కౌ ఆ ఐ ఔ'.split():
        add(word,'words')
        for end in range(1,len(word)):
            add(word[:end],'partial-input')
    for vowel in (0xc05,0xc06,0xc07,0xc08,0xc09,0xc0a,0xc0b,0xc0c,0xc0e,0xc0f,0xc10,0xc12,0xc13,0xc14,0xc60,0xc61):
        for mark in range(0xc3e,0xc4e):
            add((vowel,mark),'independent-vowels')
    for sign in (0xc48,0xc4a,0xc4b,0xc4c,0xc55,0xc56,0xc62,0xc63):
        for other in alphabet+(0xc48,0xc4a,0xc4b,0xc4c,0xc55,0xc56,0xc62,0xc63):
            add((0xc15,sign,other),'sign-tails')
            add((0xc15,other,sign),'sign-tails')
    for link in ((0xc4d,), (0xc4d,0x200d),(0x200d,),(0xc4d,0x200c)):
        add((0xc15,*link)*40+(0xc3f,),'long')
    for sign in (0xc3f,0xc3e,0xc4b,0xc01,0xc03,0x200c):
        add((0xc15,)+(sign,)*100,'long')
    if stress:
        cases={}
        rng=random.Random(20260925)
        for _ in range(1024):
            add(tuple(rng.choice(alphabet+(0xc40,0xc47,0xc48,0xc70,0xc4e,0xc57,0xc55,0xc56,0xc62,0xc63,0xc02,0xa0,0x25cc)) for _ in range(rng.randint(1,40))),'holdout')
    if vowels:
        cases={}
        for vowel in (0xc05,0xc06,0xc0f,0xc13):
            for link in ((0x200d,),(0x200c,),(0xa0,),(0xc4d,),(0x200d,0xc4d),(0xc4d,0x200d),(0xc4d,0x200d,0xc3c)):
                for base in (0xc15,0xc30,0x25cc):
                    for mark in range(0xc3e,0xc4d):
                        add((vowel,*link,base,mark),'vowel-join-holdout')
                        add((vowel,*link,base,0xa0,mark),'vowel-nbsp-holdout')
    return list(cases.items())


def matra_corpus():
    marks = (0xc3e,0xc3f,0xc40,0xc41,0xc42,0xc43,0xc44,0xc46,0xc47,
             0xc48,0xc4a,0xc4b,0xc4c,0xc55,0xc56,0xc62,0xc63)
    cases = {(0xc15,*tail):'matra-triples' for tail in itertools.product(marks,repeat=3)}
    for prefix in ((0xc15,0xc4d,0xc15),(0xc15,0x200d),(0xc30,0xc4d),
                   (0xc05,),(0xc05,0x200d,0xc15),(0xa0,),(0x25cc,)):
        for tail in itertools.product(marks,repeat=2):
            cases[(*prefix,*tail)] = 'matra-prefixes'
    rng = random.Random(202609251)
    for _ in range(1024):
        cases[(0xc15,*(rng.choice(marks+(0xc3c,0xc4d)) for _ in range(rng.randrange(4,9))))]='matra-long-holdout'
    return list(cases.items())


def join_corpus():
    vowels = (0xc05,0xc06,0xc07,0xc08,0xc09,0xc0a,0xc0b,0xc0c,0xc0e,0xc0f,0xc10,0xc12,0xc13,0xc14,0xc60,0xc61)
    consonants = (*range(0xc15,0xc29),*range(0xc2a,0xc31),0xc32,0xc33,
                  *range(0xc35,0xc3a),0xc58,0xc59,0xc5a)
    cases = {}
    for vowel in vowels:
        for link in ((0xc4d,),(0xc3c,0xc4d),(0xc4d,0x200d),(0xc4d,0x200c),(0x200d,),(0x200d,0xc4d)):
            for consonant in consonants:
                for sign in (0xc3e,0xc48,0xc57,0xc55,0xc3c):
                    cases[(vowel,*link,consonant,sign)]='vowel-consonant-holdout'
        for sign in (*range(0xc3e,0xc4e),0xc55,0xc56,0xc62,0xc63):
            cases[(vowel,sign)]='vowel-sign-holdout'
    return list(cases.items())


def verify(args):
    started=time.perf_counter()
    cases=[(tuple(s),'explicit') for s in json.loads(args.case_file.read_text(encoding='utf-8'))] if args.case_file else join_corpus() if args.joins else matra_corpus() if args.matras else corpus(args.stress,args.vowels)
    if not cases or any(not s or len(s)>256 or any(type(c) is not int or not 0<=c<=65535 or 0xd800<=c<=0xdfff for c in s) for s,_ in cases):
        raise ValueError('Expected nonempty BMP input within observer capacity')
    inputs=[identity(args.font),identity(args.reference)]
    if args.observations:
        saved=json.loads(args.observations.read_text(encoding='utf-8'))
        if saved['inputs']!=inputs or [r['scalars'] for r in saved['rows']]!=[list(s) for s,_ in cases]:
            raise ValueError('Stale font, observer or corpus observations')
        rows=saved['rows']
    else:
        rows=observe(args,cases,seed='c15')
    (args.output_dir/'observations.json').write_text(json.dumps(dict(inputs=inputs,rows=rows)),encoding='utf-8')
    payload=b'MDVCL1\0\0'+struct.pack('<I',len(cases))
    for s,_ in cases:
        payload+=struct.pack('<I',len(s))+struct.pack('<'+'I'*len(s),*s)
    native=subprocess.run([str(args.native),'--telugu-clusters'],input=payload,capture_output=True,timeout=2,env=dict(os.environ,MAICORD_TELUGU_CLUSTER_STREAM='1',MAICORD_TELUGU_FONT=str(args.font)),creationflags=subprocess.CREATE_NO_WINDOW)
    if native.returncode or native.stdout[:12]!=payload[:12] or len(native.stdout)!=12+sum(len(s) for s,_ in cases):
        raise RuntimeError('Invalid native cluster response: '+repr(native.stdout[-200:]))
    cursor=12; mismatches=0; unavailable=0
    for row in rows:
        count=len(row['scalars']);row['native']=list(native.stdout[cursor:cursor+count]);cursor+=count
        row['exact']=None if 'unavailable' in row else row['native']==row['expected']
        mismatches+=row['exact'] is False;unavailable+=row['exact'] is None
    result=dict(status='failed' if mismatches else 'incomplete' if unavailable else 'passed',cases=len(rows),
        mismatches=mismatches,unavailable=unavailable,seconds=time.perf_counter()-started,inputs=inputs,native=identity(args.native),rows=rows,
        scope='Telugu input scanner versus final Windows source-group boundaries; no glyph, positioning or app parity established.')
    (args.output_dir/'result.json').write_text(json.dumps(result),encoding='utf-8')
    print(f"{len(rows)-mismatches-unavailable}/{len(rows)} exact boundary vectors, {mismatches} differences, {unavailable} unavailable in {result['seconds']:.2f}s")
    return 1 if mismatches else 2 if unavailable else 0


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for key in ('font','native','reference','output-dir'):
        parser.add_argument('--'+key,type=Path,required=True)
    parser.add_argument('--observations',type=Path)
    corpus_options=parser.add_mutually_exclusive_group()
    corpus_options.add_argument('--matras',action='store_true')
    corpus_options.add_argument('--joins',action='store_true')
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
    if args.matras:
        command.append('--matras')
    if args.joins:
        command.append('--joins')
    if args.stress:
        command.append('--stress')
    if args.vowels:
        command.append('--vowels')
    if args.case_file:
        command.extend(['--case-file',str(args.case_file.resolve())])
    try:
        row=run_check('telugu-clusters',command,Path.cwd(),args.output_dir,5,threading.Event())
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
