"""Execute saved Tibetan policy fixtures through the native scalar-run shaper.

Unlike scheduling projection, this compares actual native glyph order, source
groups and placement bits, including the previously exposed pref reordering.
"""
import argparse,json,os,struct,subprocess,sys,threading,time
from pathlib import Path
from verify_myanmar_plan import read
from gsub_execution_oracle import identity
from tamil_shape_oracle import decode

def worker(args):
    start=time.perf_counter();checks=[]
    for path in args.observations:
        saved=read(path)
        for case in saved['checks']:
            for mode in ('default','liga=0,clig=0,kern=0'):
                observed=[o for o in case['observations'] if o['mode']==mode]
                cases=[(tuple(o['scalars']),o['context']) for o in observed]
                payload=bytearray(b'MDSHP1\0\0'+struct.pack('<I',len(cases)))
                for scalars,_ in cases:payload+=struct.pack('<I',len(scalars))+struct.pack('<'+'I'*len(scalars),*scalars)
                run=subprocess.run([str(args.native)],input=payload,capture_output=True,timeout=2,
                    env=dict(os.environ,MAICORD_TIBETAN_FONT=case['font']['path'],MAICORD_TIBETAN_INDEPENDENT=str(int(mode!='default')),MAICORD_TIBETAN_RTL='0',MAICORD_TIBETAN_OWNED=str(int(args.owned)),MAICORD_TIBETAN_COMPACT='0'),creationflags=subprocess.CREATE_NO_WINDOW)
                if run.returncode:raise RuntimeError(repr(run.stdout[-500:])+repr(run.stderr))
                actual=decode(run.stdout,cases,True)
                for source,a in zip(observed,actual):
                    b=source['result'];exact=a['status']==0 and a['glyphs']==b['glyphs'] and a['clusters']==b['clusters'] and a['placements']==b['placement_bits']
                    checks.append(dict(name=case['name'],mode=mode,context=source['context'],scalars=source['scalars'],exact=exact,native=a,reference=b))
    result=dict(status='passed' if checks and all(r['exact'] for r in checks) else 'failed',cases=len(checks),exact=sum(r['exact'] for r in checks),
        seconds=time.perf_counter()-start,native=identity(args.native),source=identity(Path(__file__)),observations=[identity(p) for p in args.observations],owned=args.owned,scope=__doc__,checks=checks)
    (args.output_dir/'result.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(result['exact'],'/',result['cases'],'exact native policy shapes');return int(result['status']!='passed')

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--native',required=True,type=Path)
    p.add_argument('--observations',required=True,action='append',type=Path);p.add_argument('--output-dir',required=True,type=Path)
    p.add_argument('--owned',action='store_true',help='Exercise retained font admission and UTF-8 adapters')
    p.add_argument('--worker',action='store_true',help=argparse.SUPPRESS);args=p.parse_args()
    if args.worker:return worker(args)
    args.output_dir.mkdir(parents=True,exist_ok=True);path=args.output_dir/'result.json'
    if path.exists():p.error('Preserve existing evidence')
    path.write_text('{"status":"running"}')
    sys.path.insert(0,str(Path(__file__).resolve().parents[1]));from verify_native_text import run_check,CheckFailure
    try:r=run_check('tibetan-policy-shapes',[sys.executable,str(Path(__file__).resolve()),*sys.argv[1:],'--worker'],Path.cwd(),args.output_dir,5,threading.Event())
    except CheckFailure as error:r=error.row
    result=json.loads(path.read_text());result['execution']=r
    if r['exit']:result['status']=r['status']
    path.write_text(json.dumps(result,indent=2)+'\n');return r['exit']
if __name__=='__main__':raise SystemExit(main())
