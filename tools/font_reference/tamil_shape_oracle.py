"""Single-font Indic cluster/run checks; each subprocess has a 2s limit.

Compares glyph IDs, source maps and exact placement bits. This is not app/run,
font admission or pixel acceptance. Reference observations can be reused by hash.
"""
import argparse
import atexit
import hashlib
import json
import os
import random
from pathlib import Path
import struct
import subprocess
import sys
import threading
import time


def corpus():
    consonants=(0xb95,0xb99,0xb9a,0xb9c,0xb9e,0xb9f,0xba3,0xba4,0xba8,0xba9,0xbaa,
                0xbae,0xbaf,0xbb0,0xbb1,0xbb2,0xbb3,0xbb4,0xbb5,0xbb6,0xbb7,0xbb8,0xbb9)
    signs=(0xbbe,0xbbf,0xbc0,0xbc1,0xbc2,0xbc6,0xbc7,0xbc8,0xbca,0xbcb,0xbcc,0xbcd,0xbd7)
    cases={}
    def add(s,g):
        cases.setdefault(tuple(s),g)
    for c in consonants:
        add((c,),'nominal')
        for sign in signs:
            add((c,sign),'vowels-and-virama')
        for tail in ((0xbc6,0xbbe),(0xbc7,0xbbe),(0xbc6,0xbd7)):
            add((c,*tail),'split-input')
        for other in consonants:
            add((c,0xbcd,other),'conjuncts')
            add((c,0xbcd,other,0xbca),'conjunct-split-vowel')
    for sign in signs:
        add((sign,),'orphan-signs')
        add((0xb95,0xbcd,0xbb7,sign),'ksha-vowels')
    for s in ((0xb94,),(0xb92,0xbd7),(0xbb8,0xbcd,0xbb0,0xbc0),
              (0xbb6,0xbcd,0xbb0,0xbc0),(0xb95,0xbcd,0x200c),(0xb95,0xbcd,0x200d)):
        add(s,'specials')
    return list(cases.items())


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def stress_corpus(seed=20260920):
    rng=random.Random(seed)
    alphabet=(0xb95,0xbb0,0xbb7,0xb85,0xb94,0xbcd,0x200c,0x200d,0xbc6,0xbc7,0xbc8,
              0xbc0,0xbbf,0xbbe,0xbc1,0xbc2,0xbca,0xbcb,0xbcc,0xb82,0xbd7,0xa0,0x25cc)
    return [(tuple(rng.choice(alphabet) for _ in range(rng.randint(1,24))),'random-runs') for _ in range(512)]


def decode(data,cases,native):
    magic=b'MDSHP1\0\0' if native else b'MCSHPB1\0'
    if data[:12]!=magic+struct.pack('<I',len(cases)):
        raise ValueError('Invalid response header')
    cursor=12;rows=[]
    for scalars,_ in cases:
        if native:
            status,count=struct.unpack_from('<II',data,cursor);cursor+=8
        else:
            count=struct.unpack_from('<I',data,cursor)[0];cursor+=4;status=0
        glyphs=list(struct.unpack_from('<'+'H'*count,data,cursor));cursor+=count*2
        unit='I' if native else 'H'
        clusters=list(struct.unpack_from('<'+unit*len(scalars),data,cursor));cursor+=len(scalars)*(4 if native else 2)
        placements=[list(struct.unpack_from('<III',data,cursor+i*12)) for i in range(count)];cursor+=count*12
        rows.append(dict(status=status,glyphs=glyphs,clusters=clusters,placements=placements))
    if cursor!=len(data):
        raise ValueError('Truncated or trailing response')
    return rows


def observe(args,cases,inputs,prior,*,seed='b95'):
    rows=list(prior);deadline=time.monotonic()+5
    def save():
        (args.output_dir/'reference.json').write_text(json.dumps(dict(inputs=inputs,rows=rows))+'\n',encoding='utf-8')
    def budget():
        left=deadline-time.monotonic()
        if left<=0:
            raise TimeoutError('Whole reference observation deadline; completed records saved')
        return min(2,left)
    while len(rows)<len(cases):
        remaining=cases[len(rows):]
        payload=bytearray(b'MCSHPB1\0'+struct.pack('<I',len(remaining)))
        for s,_ in remaining:
            payload+=struct.pack('<II',len(s),2 if args.rtl else 0)+struct.pack('<'+'H'*len(s),*s)
        ref=subprocess.run([str(args.reference.resolve()),str(args.font.resolve()),seed,'default','1148846080','0','0','en-US'],
            input=payload,capture_output=True,timeout=budget(),env=dict(os.environ,MAICORD_SHAPE_BATCH='1'),creationflags=subprocess.CREATE_NO_WINDOW)
        if ref.stdout[:12]!=payload[:12]:
            raise ValueError('Invalid reference response header')
        cursor=12;completed=0
        for s,_ in remaining:
            if cursor==len(ref.stdout):
                break
            count=struct.unpack_from('<I',ref.stdout,cursor)[0];cursor+=4+count*14+len(s)*2
            if cursor>len(ref.stdout):
                raise ValueError('Truncated reference record')
            completed+=1
        rows.extend(decode(ref.stdout[:8]+struct.pack('<I',completed)+ref.stdout[12:],remaining[:completed],False));save()
        if not ref.returncode:
            if len(rows)!=len(cases):
                raise ValueError('Missing successful reference records')
            break
        if len(rows)==len(cases):
            raise ValueError('Reference failed after final record')
        scalars,_=cases[len(rows)]
        single=subprocess.run([str(args.reference.resolve()),str(args.font.resolve()),','.join(f'{c:x}' for c in scalars),
            'default','1148846080','1' if args.rtl else '0','0','en-US'],capture_output=True,timeout=budget(),creationflags=subprocess.CREATE_NO_WINDOW)
        failed=json.loads(single.stdout)
        if not (failed['hresult'] or failed['placement_hresult']):
            raise RuntimeError('Reference batch failure was not reproduced')
        rows.append(dict(unavailable=failed));save()
    return rows


def bounded_bengali(args):
    args.output_dir.mkdir(parents=True,exist_ok=True)
    result_path=args.output_dir/'result.json'
    result_path.write_text('{"status":"running"}',encoding='utf-8')
    sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
    from verify_native_text import CheckFailure,run_check
    command=[sys.executable,str(Path(__file__).resolve()),*sys.argv[1:],'--worker']
    try:
        execution=run_check('malayalam-shape' if args.malayalam else 'kannada-shape' if args.kannada else 'telugu-shape' if args.telugu else 'odia-shape' if args.odia else 'gurmukhi-shape' if args.gurmukhi else 'gujarati-shape' if args.gujarati else 'bengali-shape',command,Path.cwd(),args.output_dir,5,threading.Event())
    except CheckFailure as error:
        execution=error.row
    result=json.loads(result_path.read_text(encoding='utf-8'));result['execution']=execution
    if execution['exit'] and result.get('status')=='running':
        result['status']=execution['status']
    result_path.write_text(json.dumps(result,indent=2),encoding='utf-8')
    print({k:v for k,v in result.items() if k in ('status','cases','exact','reference_unavailable','seconds')})
    return execution['exit']


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('font','native','reference','output-dir'):
        parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--observations',type=Path)
    parser.add_argument('--group',action='append')
    parser.add_argument('--case',action='append')
    parser.add_argument('--case-file',type=Path,help='JSON array of scalar arrays for a bounded diagnostic corpus.')
    parser.add_argument('--run',action='store_true',help='Use the larger run probe capacity and enable parser/stress corpora; all cases use the same run executor.')
    parser.add_argument('--bengali',action='store_true',help='Compare the Bengali single-cluster executor and its corpus.')
    parser.add_argument('--malayalam',action='store_true',help='Compare experimental Malayalam original-cluster execution.')
    parser.add_argument('--kannada',action='store_true',help='Compare Kannada original-cluster execution.')
    parser.add_argument('--telugu',action='store_true',help='Compare Telugu original-cluster execution.')
    parser.add_argument('--odia',action='store_true',help='Compare Odia original-cluster execution.')
    parser.add_argument('--capacity',action='store_true',help='Odia 31-slot preparation, circles and split-vowel cutoff corpus.')
    parser.add_argument('--gurmukhi',action='store_true',help='Compare Gurmukhi original-cluster execution.')
    parser.add_argument('--gujarati',action='store_true',help='Compare Gujarati cluster or owned run execution.')
    parser.add_argument('--rtl',action='store_true',help='Use RTL positioning for Bengali, Gujarati, Gurmukhi or Odia.')
    parser.add_argument('--compact',action='store_true',help='Use Indic compact publication with exact-prefix buffer aliases.')
    parser.add_argument('--check-missing-rejection',action='store_true',help='With --compact, separately verify whole-result rejection when the reference contains glyph zero; never count it as visual parity.')
    parser.add_argument('--bengali-complex','--complex',action='store_true',help='Use three-consonant, mark and control clusters.')
    parser.add_argument('--worker',action='store_true',help=argparse.SUPPRESS)
    parser.add_argument('--corpus',choices=('clusters','parser','stress','vowels','matras','joins','modifiers'),default='clusters')
    parser.add_argument('--seed',type=int,default=20260920,help='Deterministic stress-corpus seed.')
    args=parser.parse_args();started=time.perf_counter()
    if sum((args.bengali,args.gujarati,args.gurmukhi,args.odia,args.telugu,args.kannada,args.malayalam))>1:parser.error('Select one script')
    if args.kannada and args.corpus!='clusters' and not args.run:parser.error('Kannada parser/stress inputs require --run')
    if args.telugu and args.corpus!='clusters' and not args.run:parser.error('Telugu parser/stress inputs require --run')
    if args.capacity and (not (args.odia or args.telugu or args.kannada or args.malayalam) or args.bengali_complex):parser.error('--capacity requires Odia or Telugu without --complex')
    if args.odia and args.corpus!='clusters' and not args.run:parser.error('Odia parser/stress inputs require --run')
    if args.corpus in ('vowels','matras','joins','modifiers') and not (args.odia or args.telugu or args.kannada or args.malayalam):parser.error('This corpus requires --odia or --telugu with --run')
    if args.rtl and not (args.bengali or args.gujarati or args.gurmukhi or args.odia or args.telugu or args.kannada or args.malayalam):parser.error('--rtl requires an Indic profile')
    if args.compact and not ((args.bengali or args.gujarati or args.gurmukhi or args.odia or args.telugu or args.kannada or args.malayalam) and args.run):parser.error('--compact requires an Indic run')
    if args.check_missing_rejection and not args.compact:parser.error('--check-missing-rejection requires --compact')
    if args.bengali_complex and not (args.bengali or args.gujarati or args.gurmukhi or args.odia or args.telugu or args.kannada or args.malayalam):
        parser.error('--complex requires an Indic profile')
    if (args.bengali or args.gujarati or args.gurmukhi or args.odia or args.telugu or args.kannada or args.malayalam) and not args.worker:
        return bounded_bengali(args)
    if args.malayalam and (args.capacity or args.bengali_complex or args.corpus!='clusters'):
        parser.error('Select Malayalam clusters or an explicit bounded run corpus')
    if args.malayalam:
        from malayalam_shape_samples import corpus as malayalam_corpus
        selected=malayalam_corpus()
    elif args.kannada:
        from kannada_shape_samples import corpus as kannada_corpus, complex_corpus, capacity_corpus
        selected=capacity_corpus() if args.capacity else complex_corpus() if args.bengali_complex else kannada_corpus()
        if args.corpus!='clusters':
            from kannada_cluster_oracle import corpus as kannada_parser_corpus, matra_corpus, join_corpus
            if args.corpus=='matras':selected=matra_corpus()
            elif args.corpus=='joins':selected=join_corpus()
            elif args.corpus=='modifiers':parser.error('Kannada modifier corpus is not defined')
            else:selected=kannada_parser_corpus(args.corpus=='stress',vowels=args.corpus=='vowels')
    elif args.telugu:
        from telugu_shape_samples import corpus as telugu_corpus, complex_corpus, capacity_corpus
        selected=capacity_corpus() if args.capacity else complex_corpus() if args.bengali_complex else telugu_corpus()
        if args.corpus!='clusters':
            from telugu_cluster_oracle import corpus as telugu_parser_corpus, matra_corpus, join_corpus
            if args.corpus=='matras':selected=matra_corpus()
            elif args.corpus=='joins':selected=join_corpus()
            elif args.corpus=='modifiers':parser.error('Telugu modifier corpus is not defined')
            else:selected=telugu_parser_corpus(args.corpus=='stress',vowels=args.corpus=='vowels')
    elif args.odia:
        from odia_shape_samples import corpus as odia_corpus, complex_corpus, capacity_corpus
        selected=capacity_corpus() if args.capacity else complex_corpus() if args.bengali_complex else odia_corpus()
        if args.corpus!='clusters':
            from odia_cluster_oracle import corpus as odia_parser_corpus, matra_corpus, join_corpus
            from odia_shape_samples import modifier_corpus
            if args.corpus=='matras':selected=matra_corpus()
            elif args.corpus=='joins':selected=join_corpus()
            elif args.corpus=='modifiers':selected=modifier_corpus()
            else:selected=odia_parser_corpus(args.corpus=='stress',vowels=args.corpus=='vowels')
    elif args.gurmukhi:
        if args.corpus!='clusters' and not args.run:parser.error('Gurmukhi parser/stress inputs require --run')
        from gurmukhi_shape_samples import corpus as gurmukhi_corpus, complex_corpus
        selected=complex_corpus() if args.bengali_complex else gurmukhi_corpus()
        if args.corpus!='clusters':
            from gurmukhi_cluster_oracle import corpus as gurmukhi_parser_corpus
            selected=gurmukhi_parser_corpus(args.corpus=='stress')
    elif args.gujarati:
        if args.corpus!='clusters' and not args.run:parser.error('Gujarati parser/stress inputs require --run')
        from gujarati_shape_samples import corpus as gujarati_corpus, complex_corpus
        selected=complex_corpus() if args.bengali_complex else gujarati_corpus()
        if args.corpus!='clusters':
            from gujarati_cluster_oracle import corpus as gujarati_parser_corpus
            selected=gujarati_parser_corpus(args.corpus=='stress')
    elif args.bengali:
        if args.corpus!='clusters' and not args.run:
            parser.error('Bengali parser/stress inputs require --run')
        from bengali_shape_samples import corpus as bengali_corpus, complex_corpus
        selected=complex_corpus() if args.bengali_complex else bengali_corpus()
        if args.corpus!='clusters':
            from bengali_cluster_oracle import corpus as bengali_parser_corpus
            selected=bengali_parser_corpus(args.corpus=='stress')
    elif args.corpus in ('parser','stress'):
        if not args.run:
            parser.error('--corpus parser/stress requires --run')
        from tamil_cluster_oracle import corpus as parser_corpus
        selected=parser_corpus() if args.corpus=='parser' else stress_corpus(args.seed)
    else:
        selected=corpus()
    cases=[(tuple(int(c,16) for c in s.split(',')),'explicit') for s in args.case] if args.case else selected
    if args.case_file:
        if args.case:parser.error('--case and --case-file are mutually exclusive')
        cases=[(tuple(s),'explicit') for s in json.loads(args.case_file.read_text(encoding='utf-8'))]
    if args.group:
        cases=[(s,g) for s,g in cases if g in args.group]
    if not cases or any(not s or len(s)>(256 if args.run else 31) or any(c>65535 or 0xd800<=c<=0xdfff for c in s) for s,_ in cases):
        raise ValueError('Expected nonempty BMP inputs within observer capacity')
    args.output_dir.mkdir(parents=True,exist_ok=True)
    result_path=args.output_dir/'result.json'
    result_path.write_text(json.dumps(dict(status='running',cases=len(cases)))+'\n',encoding='utf-8')
    def unfinished():
        result_path.write_text(json.dumps(dict(status='failed',cases=len(cases),seconds=time.perf_counter()-started,
            reason='Comparison did not complete; no parity claim.'))+'\n',encoding='utf-8')
    atexit.register(unfinished)
    inputs=dict(font=digest(args.font),reference=digest(args.reference),cases=[list(s) for s,_ in cases])
    if args.bengali or args.gujarati or args.gurmukhi or args.odia or args.telugu or args.kannada or args.malayalam:
        inputs['script']='mlm2' if args.malayalam else 'knd2' if args.kannada else 'tel2' if args.telugu else 'ory2' if args.odia else 'gur2' if args.gurmukhi else 'gjr2' if args.gujarati else 'bng2'
    if args.rtl:inputs['rtl']=True
    if args.observations:
        saved=json.loads(args.observations.read_text(encoding='utf-8'))
        if saved['inputs']!=inputs:
            raise ValueError('Stale font, reference or corpus observations')
        expected=saved['rows']
        if len(expected)>len(cases):
            raise ValueError('Extra reference rows')
    else:
        expected=[]
    reused=len(expected)
    if len(expected)<len(cases):
        expected=observe(args,cases,inputs,expected,seed='d15' if args.malayalam else 'c95' if args.kannada else 'c15' if args.telugu else 'b15' if args.odia else 'a15' if args.gurmukhi else 'a95' if args.gujarati else '995' if args.bengali else 'b95')
    for (scalars,_),row in zip(cases,expected):
        if 'unavailable' in row:
            if set(row)!={'unavailable'} or not (row['unavailable'].get('hresult') or row['unavailable'].get('placement_hresult')):
                raise ValueError('Invalid unavailable reference row')
            continue
        if set(row)!={'status','glyphs','clusters','placements'} or row['status'] or len(row['clusters'])!=len(scalars) or len(row['placements'])!=len(row['glyphs']):
            raise ValueError('Invalid reference row dimensions')
        if any(type(g) is not int or not 0<=g<=65535 for g in row['glyphs']) or any(type(c) is not int or not 0<=c<max(1,len(row['glyphs'])) for c in row['clusters']):
            raise ValueError('Invalid reference glyph/source map')
        if any(len(p)!=3 or any(type(v) is not int or not 0<=v<=0xffffffff for v in p) for p in row['placements']):
            raise ValueError('Invalid reference placement bits')
    (args.output_dir/'reference.json').write_text(json.dumps(dict(inputs=inputs,rows=expected))+'\n',encoding='utf-8')
    payload=bytearray(b'MDSHP1\0\0'+struct.pack('<I',len(cases)))
    for s,_ in cases:
        payload+=struct.pack('<I',len(s))+struct.pack('<'+'I'*len(s),*s)
    native=subprocess.run([str(args.native.resolve()),str(args.font.resolve())],input=payload,capture_output=True,timeout=2,
        env=dict(os.environ,MAICORD_MALAYALAM_RUN='1' if args.malayalam and args.run else '0',MAICORD_MALAYALAM_BATCH='1' if args.malayalam else '0',MAICORD_KANNADA_RUN='1' if args.kannada and args.run else '0',MAICORD_KANNADA_BATCH='1' if args.kannada else '0',MAICORD_TELUGU_RUN='1' if args.telugu and args.run else '0',MAICORD_TELUGU_BATCH='1' if args.telugu else '0',MAICORD_ODIA_RUN='1' if args.odia and args.run else '0',MAICORD_ODIA_BATCH='1' if args.odia else '0',MAICORD_GURMUKHI_RUN='1' if args.gurmukhi and args.run else '0',MAICORD_GURMUKHI_BATCH='1' if args.gurmukhi else '0',MAICORD_GUJARATI_RUN='1' if args.gujarati and args.run else '0',MAICORD_GUJARATI_BATCH='1' if args.gujarati else '0',MAICORD_BENGALI_COMPACT='1' if args.compact else '0',MAICORD_DEVANAGARI_RTL='1' if args.rtl else '0',MAICORD_DEVANAGARI_BATCH='0',MAICORD_DEVANAGARI_MEMO_BENCH='0',MAICORD_BENGALI_BATCH='1' if args.bengali else '0',
            MAICORD_BENGALI_RUN='1' if args.bengali and args.run else '0',MAICORD_TAMIL_BATCH='0' if args.run or args.bengali or args.gujarati or args.gurmukhi or args.odia or args.telugu or args.kannada or args.malayalam else '1',MAICORD_TAMIL_RUN='1' if args.run and not (args.bengali or args.gujarati or args.gurmukhi or args.odia or args.telugu or args.kannada or args.malayalam) else '0'),creationflags=subprocess.CREATE_NO_WINDOW)
    if native.returncode:
        raise RuntimeError(f'Native failed: {native.stdout[-500:]!r}')
    actual=decode(native.stdout,cases,True);differences=[];groups={};unavailable=0;rejected=0
    for (s,g),a,e in zip(cases,actual,expected):
        if 'unavailable' in e:
            unavailable+=1
            groups.setdefault(g,dict(exact=0,different=0))['unavailable']=groups.get(g,{}).get('unavailable',0)+1
            continue
        if args.check_missing_rejection and 0 in e['glyphs']:
            # SfntStatus::unsupported_sequence (8) hands the entire run to fallback.
            if a['status']==8 and not a['glyphs'] and not a['placements'] and not any(a['clusters']):
                rejected+=1
                counts=groups.setdefault(g,dict(exact=0,different=0));counts['missing_rejected']=counts.get('missing_rejected',0)+1
                continue
            keys=['missing_glyph_rejection']
        else:keys=[k for k in e if a[k]!=e[k]]
        counts=groups.setdefault(g,dict(exact=0,different=0));counts['different' if keys else 'exact']+=1
        if keys:
            differences.append(dict(scalars=list(s),group=g,keys=keys,actual=a,expected=e))
    result=dict(status='failed' if differences else 'incomplete' if unavailable else 'passed',cases=len(cases),exact=len(cases)-len(differences)-unavailable-rejected,reference_unavailable=unavailable,missing_glyph_rejected=rejected,
        reference_reused=reused,reference_observed=len(cases)-reused,compact=args.compact,seconds=time.perf_counter()-started,native_sha256=digest(args.native),groups=groups,differences=differences,
        scope='One selected font, default typography, '+('RTL' if args.rtl else 'LTR')+'; '+(('Malayalam UTF-8 through owned font plan' if args.run else 'Malayalam original scalar cluster') if args.malayalam else ('Kannada UTF-8 through owned font plan' if args.run else 'Kannada original scalar cluster') if args.kannada else ('Telugu UTF-8 through owned font plan' if args.run else 'Telugu original scalar cluster') if args.telugu else ('Odia UTF-8 through owned font plan' if args.run else 'Odia original scalar cluster') if args.odia else ('Gurmukhi UTF-8 through owned font plan' if args.run else 'Gurmukhi original scalar cluster') if args.gurmukhi else ('Gujarati UTF-8 through owned font plan' if args.run else 'Gujarati original scalar cluster') if args.gujarati else ('Bengali UTF-8 through owned font plan' if args.run else 'Bengali original scalar cluster') if args.bengali else 'Tamil UTF-8 through owned font plan')+'; exact glyph/source/placement output, not app parity')
    result_path.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    atexit.unregister(unfinished)
    print(json.dumps({k:v for k,v in result.items() if k!='differences'}))
    for r in differences[:12]:
        print(','.join(f'{c:x}' for c in r['scalars']),r['keys'],
            'glyph counts',len(r['actual']['glyphs']),len(r['expected']['glyphs']))
    return 1 if differences else 2 if unavailable else 0


if __name__=='__main__':
    raise SystemExit(main())
