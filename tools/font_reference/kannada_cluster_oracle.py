"""Bounded Kannada scanner comparisons; final Windows clusters constrain parsing only."""
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
    alphabet = (0xc95,0xcb0,0xc85,0xcbc,0xccd,0x200c,0x200d,0xcbf,0xcc1,0xcbe,0xccb,0xccc,0xc81,0xc83)
    for scalar in range(0xc80,0xd00):
        for s in ((scalar,), (0xc95,scalar), (scalar,0xccd,0xc95), (0xc95,scalar,0xcbf), (0xc95,scalar,scalar)):
            add(s,'scalar-coverage')
    assert {s[0] for s,g in cases.items() if g=='scalar-coverage' and len(s)==1}==set(range(0xc80,0xd00))
    for base in (0xc95,0xcb0,0xc85,0xa0,0x25cc,0xcbf,0x200d):
        for pair in itertools.product(alphabet,repeat=2):
            add((base,*pair),'pairs')
    for prefix in ((0xc95,0xccd),(0xc95,0xcbf),(0xcb0,0xccd),(0xc95,0xccd,0x200d),(0xc95,0xccd,0x200c)):
        for pair in itertools.product(alphabet,repeat=2):
            add((*prefix,*pair),'tails')
    for word in 'ಕನ್ನಡ ಭಾಷೆ ಭಾರತ ನಮಸ್ಕಾರ ಮಿತ್ರ ಸುದ್ದಿ ಕಂಪ್ಯೂಟರ್ ಬೆಂಗಳೂರು ಪ್ರಾರ್ಥನೆ ಶ್ರದ್ಧೆ ಸೂರ್ಯ ಲಕ್ಷ್ಮೀ ಕ್ಷ ಜ್ಞ ರ್ಕ ಕೈ ಕೌ ಆ ಐ ಔ'.split():
        add(word,'words')
        for end in range(1,len(word)):
            add(word[:end],'partial-input')
    for vowel in (0xc85,0xc86,0xc87,0xc88,0xc89,0xc8a,0xc8b,0xc8c,0xc8e,0xc8f,0xc90,0xc92,0xc93,0xc94,0xce0,0xce1):
        for mark in range(0xcbe,0xcce):
            add((vowel,mark),'independent-vowels')
    for sign in (0xcc8,0xcca,0xccb,0xccc,0xcd5,0xcd6,0xce2,0xce3):
        for other in alphabet+(0xcc8,0xcca,0xccb,0xccc,0xcd5,0xcd6,0xce2,0xce3):
            add((0xc95,sign,other),'sign-tails')
            add((0xc95,other,sign),'sign-tails')
    for link in ((0xccd,), (0xccd,0x200d),(0x200d,),(0xccd,0x200c)):
        add((0xc95,*link)*40+(0xcbf,),'long')
    for sign in (0xcbf,0xcbe,0xccb,0xc81,0xc83,0x200c):
        add((0xc95,)+(sign,)*100,'long')
    if stress:
        cases={}
        rng=random.Random(20260925)
        for _ in range(1024):
            add(tuple(rng.choice(alphabet+(0xcc0,0xcc7,0xcc8,0xcf0,0xcce,0xcd7,0xcd5,0xcd6,0xce2,0xce3,0xc82,0xa0,0x25cc)) for _ in range(rng.randint(1,40))),'holdout')
    if vowels:
        cases={}
        for vowel in (0xc85,0xc86,0xc8f,0xc93):
            for link in ((0x200d,),(0x200c,),(0xa0,),(0xccd,),(0x200d,0xccd),(0xccd,0x200d),(0xccd,0x200d,0xcbc)):
                for base in (0xc95,0xcb0,0x25cc):
                    for mark in range(0xcbe,0xccd):
                        add((vowel,*link,base,mark),'vowel-join-holdout')
                        add((vowel,*link,base,0xa0,mark),'vowel-nbsp-holdout')
    return list(cases.items())


def matra_corpus():
    marks = (0xcbe,0xcbf,0xcc0,0xcc1,0xcc2,0xcc3,0xcc4,0xcc6,0xcc7,
             0xcc8,0xcca,0xccb,0xccc,0xcd5,0xcd6,0xce2,0xce3)
    cases = {(0xc95,*tail):'matra-triples' for tail in itertools.product(marks,repeat=3)}
    for prefix in ((0xc95,0xccd,0xc95),(0xc95,0x200d),(0xcb0,0xccd),
                   (0xc85,),(0xc85,0x200d,0xc95),(0xa0,),(0x25cc,)):
        for tail in itertools.product(marks,repeat=2):
            cases[(*prefix,*tail)] = 'matra-prefixes'
    rng = random.Random(202609251)
    for _ in range(1024):
        cases[(0xc95,*(rng.choice(marks+(0xcbc,0xccd)) for _ in range(rng.randrange(4,9))))]='matra-long-holdout'
    return list(cases.items())


def join_corpus():
    vowels = (0xc85,0xc86,0xc87,0xc88,0xc89,0xc8a,0xc8b,0xc8c,0xc8e,0xc8f,0xc90,0xc92,0xc93,0xc94,0xce0,0xce1)
    consonants = (*range(0xc95,0xca9),*range(0xcaa,0xcb1),0xcb2,0xcb3,
                  *range(0xcb5,0xcba),0xcde,0xcdd)
    cases = {}
    for vowel in vowels:
        for link in ((0xccd,),(0xcbc,0xccd),(0xccd,0x200d),(0xccd,0x200c),(0x200d,),(0x200d,0xccd)):
            for consonant in consonants:
                for sign in (0xcbe,0xcc8,0xcd7,0xcd5,0xcbc):
                    cases[(vowel,*link,consonant,sign)]='vowel-consonant-holdout'
        for sign in (*range(0xcbe,0xcce),0xcd5,0xcd6,0xce2,0xce3):
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
        rows=observe(args,cases,seed='c95')
    (args.output_dir/'observations.json').write_text(json.dumps(dict(inputs=inputs,rows=rows)),encoding='utf-8')
    payload=bytearray(b'MDVCL1\0\0'+struct.pack('<I',len(cases)))
    for s,_ in cases:
        payload+=struct.pack('<I',len(s))+struct.pack('<'+'I'*len(s),*s)
    native=subprocess.run([str(args.native),'--kannada-clusters'],input=payload,capture_output=True,timeout=2,env=dict(os.environ,MAICORD_KANNADA_CLUSTER_STREAM='1',MAICORD_KANNADA_FONT=str(args.font)),creationflags=subprocess.CREATE_NO_WINDOW)
    if native.returncode or native.stdout[:12]!=payload[:12] or len(native.stdout)!=12+sum(len(s) for s,_ in cases):
        raise RuntimeError('Invalid native cluster response: '+repr(native.stdout[-200:]))
    cursor=12; mismatches=0; unavailable=0
    for row in rows:
        count=len(row['scalars']);row['native']=list(native.stdout[cursor:cursor+count]);cursor+=count
        row['exact']=None if 'unavailable' in row else row['native']==row['expected']
        mismatches+=row['exact'] is False;unavailable+=row['exact'] is None
    result=dict(status='failed' if mismatches else 'incomplete' if unavailable else 'passed',cases=len(rows),
        mismatches=mismatches,unavailable=unavailable,seconds=time.perf_counter()-started,inputs=inputs,native=identity(args.native),rows=rows,
        scope='Kannada input scanner versus final Windows source-group boundaries; no glyph, positioning or app parity established.')
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
        row=run_check('kannada-clusters',command,Path.cwd(),args.output_dir,5,threading.Event())
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
