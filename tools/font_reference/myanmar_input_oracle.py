"""Bounded Myanmar/Tibetan input boundary observations/replay; offline QA only.

Final Windows source groups constrain input segmentation, not glyph/placement,
reordering, rasterization or app parity. Saved observations are reusable.
"""
import argparse,json,os,struct,subprocess,sys,threading,time
from pathlib import Path
from tamil_cluster_oracle import observe,identity
from myanmar_input_samples import corpus
from devanagari_plan_oracle import batch_glyphs

def worker(args):
    selected_corpus=corpus
    if args.profile=='tibetan':
        from tibetan_input_samples import corpus as selected_corpus
    seed='0f40' if args.profile=='tibetan' else '1000'
    start=time.perf_counter();cases=[(tuple(s),'explicit') for s in json.loads(args.case_file.read_text())] if args.case_file else selected_corpus(args.stress)
    if not cases or any(not s or sum(1+(c>65535) for c in s)>256 or any(type(c) is not int or not 0<=c<=0x10ffff or 0xd800<=c<=0xdfff for c in s) for s,g in cases):raise ValueError('Invalid scalar cases or observer capacity')
    inputs=[identity(args.font),identity(args.reference)]
    if args.observations:
        saved=json.loads(args.observations.read_text(encoding='utf-8'))
        if saved.get('profile','myanmar')!=args.profile or saved['inputs']!=inputs or [r['scalars'] for r in saved['rows']]!=[list(s) for s,g in cases]:raise RuntimeError('Reference input changed')
        rows=saved['rows']
    else:
        rows=[];at=0
        while at<len(cases):
            end=at;units=0
            while end<len(cases) and end-at<256 and units+len(cases[end][0])<=8192:
                units+=len(cases[end][0]);end+=1
            portion=cases[at:end]
            if any(c>65535 for s,g in portion for c in s):
                observed=batch_glyphs(args.reference,args.font,[s for s,g in portion],seed=seed,detailed=True,timeout=2)
                for (s,g),o in zip(portion,observed):
                    source=0;previous=None;expected=[]
                    for c in s:
                        cluster=o['clusters'][source];expected.append(int(cluster!=previous));previous=cluster
                        if c>65535 and o['clusters'][source+1]!=cluster:raise RuntimeError('Surrogate source split')
                        source+=1+(c>65535)
                    rows.append(dict(scalars=list(s),group=g,expected=expected,source_clusters=o['clusters']))
            else:rows.extend(observe(args,portion,seed=seed))
            at=end
        (args.output_dir/'observations.json').write_text(json.dumps(dict(profile=args.profile,inputs=inputs,rows=rows)),encoding='utf-8')
    status='observed';exact=0
    if args.native:
        payload=bytearray(b'MDVCL1\0\0'+struct.pack('<I',len(cases)))
        for s,g in cases:payload+=struct.pack('<I',len(s))+struct.pack('<'+'I'*len(s),*s)
        run=subprocess.run([str(args.native)],input=payload,capture_output=True,timeout=2,
            env=dict(os.environ,**{('MAICORD_TIBETAN_INPUT_STREAM' if args.profile=='tibetan' else 'MAICORD_MYANMAR_INPUT_STREAM'):'1'}),creationflags=subprocess.CREATE_NO_WINDOW)
        if run.returncode or run.stdout[:12]!=payload[:12] or len(run.stdout)!=12+sum(len(s) for s,g in cases):raise RuntimeError('Invalid native input stream')
        at=12
        for row in rows:
            n=len(row['scalars']);row['native']=list(run.stdout[at:at+n]);at+=n
            row['exact']=None if 'unavailable' in row else row['native']==row['expected'];exact+=row['exact'] is True
        status='passed' if exact==len(cases) else 'failed'
    result=dict(status=status,profile=args.profile,cases=len(rows),exact=exact,unavailable=sum('unavailable' in r for r in rows),seconds=time.perf_counter()-start,
        inputs=inputs,native=identity(args.native) if args.native else None,scope=__doc__,rows=rows)
    (args.output_dir/'result.json').write_text(json.dumps(result),encoding='utf-8')
    print(status,len(rows),'cases',exact,'exact',round(result['seconds'],2),'seconds');return int(status=='failed')

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--profile',choices=('myanmar','tibetan'),default='myanmar')
    for key in ('font','reference','output-dir'):p.add_argument('--'+key,required=True,type=Path)
    for key in ('native','observations','case-file'):p.add_argument('--'+key,type=Path)
    p.add_argument('--stress',action='store_true');p.add_argument('--worker',action='store_true',help=argparse.SUPPRESS);args=p.parse_args()
    if args.worker:return worker(args)
    args.output_dir.mkdir(parents=True,exist_ok=True);path=args.output_dir/'result.json'
    if path.exists():p.error('Preserve existing evidence; select an unused evidence directory')
    if args.observations and args.observations.resolve() in (path.resolve(),(args.output_dir/'observations.json').resolve()):p.error('Preserve saved observations')
    path.write_text('{"status":"running"}',encoding='utf-8')
    sys.path.insert(0,str(Path(__file__).resolve().parents[1]));from verify_native_text import run_check,CheckFailure
    try:execution=run_check('myanmar-input',[sys.executable,str(Path(__file__).resolve()),*sys.argv[1:],'--worker'],Path.cwd(),args.output_dir,5,threading.Event())
    except CheckFailure as error:execution=error.row
    result=json.loads(path.read_text(encoding='utf-8'));result['execution']=execution
    if execution['exit']:result['status']=execution['status']
    path.write_text(json.dumps(result),encoding='utf-8');return execution['exit']
if __name__=='__main__':raise SystemExit(main())
