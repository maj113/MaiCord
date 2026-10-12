"""Replay saved Windows Khmer admission/font fallback results through owned UTF-8.

Each batch contains at most 32 native children, each limited to two seconds;
the complete batch has a five-second process-tree deadline.
"""
import argparse,json,os,struct,subprocess,sys,threading
from pathlib import Path
from tamil_shape_oracle import decode
from devanagari_plan_oracle import identity
from khmer_plan_policy import CONTEXTS

def worker(args):
    cases=[]
    for source in args.observations:
        saved=json.loads(source.read_text())
        if identity(args.reference)['sha256']!=saved['reference']['sha256']:raise ValueError('Reference changed')
        for row in saved['checks']:
            font=Path(row['font']['path'])
            if identity(font)['sha256']!=row['font']['sha256']:raise ValueError('Font changed')
            for independent in (False,True):
                observations=[x for x in row['observations'] if (x['mode']!='default')==independent]
                scalars=[(tuple(int(c,16) for c in CONTEXTS[x['context']].split(',')),x['context']) for x in observations]
                cases.append((font,independent,scalars,observations))
    rows=[]
    for font,independent,scalars,expected in cases[args.batch*32:(args.batch+1)*32]:
        payload=bytearray(b'MDSHP1\0\0'+struct.pack('<I',len(scalars)))
        for s,_ in scalars:payload+=struct.pack('<I',len(s))+struct.pack('<'+'I'*len(s),*s)
        process=subprocess.run([str(args.native)],input=payload,capture_output=True,timeout=2,
            env=dict(os.environ,MAICORD_KHMER_FONT=str(font),MAICORD_KHMER_OWNED='1',MAICORD_KHMER_INDEPENDENT=str(int(independent)),MAICORD_KHMER_COMPACT='0'),creationflags=subprocess.CREATE_NO_WINDOW)
        if process.returncode:raise RuntimeError(str(font)+repr(process.stdout)+repr(process.stderr))
        actual=decode(process.stdout,scalars,True)
        for (s,_),a,b in zip(scalars,actual,expected):
            reference=dict(glyphs=b['result']['glyphs'],clusters=b['result']['clusters'],placements=b['result']['placement_bits'])
            exact=a['status']==0 and all(a[k]==reference[k] for k in reference)
            rows.append(dict(font=str(font),independent=independent,scalars=s,native=a,reference=reference,exact=exact))
    if not rows:raise ValueError('Empty batch')
    result=dict(status='passed' if all(x['exact'] for x in rows) else 'failed',cases=len(rows),exact=sum(x['exact'] for x in rows),native=identity(args.native),reference=identity(args.reference),observations=[identity(x) for x in args.observations],rows=rows)
    (args.output_dir/'result.json').write_text(json.dumps(result,indent=2));print(result['exact'], '/', len(rows))
    return 0 if result['status']=='passed' else 1

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('reference','native','output-dir'):p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--observations',type=Path,action='append',required=True)
    p.add_argument('--batch',type=int,choices=range(4),required=True);p.add_argument('--worker',action='store_true',help=argparse.SUPPRESS)
    args=p.parse_args();args.output_dir.mkdir(parents=True,exist_ok=True)
    if args.worker:return worker(args)
    sys.path.insert(0,str(Path(__file__).resolve().parents[1]));from verify_native_text import run_check,CheckFailure
    path=args.output_dir/'result.json';path.write_text('{"status":"running"}')
    try:execution=run_check('khmer-admission',[sys.executable,str(Path(__file__).resolve()),*sys.argv[1:],'--worker'],Path.cwd(),args.output_dir,5,threading.Event())
    except CheckFailure as error:execution=error.row
    result=json.loads(path.read_text());result['execution']=execution
    if execution['exit']:result['status']=execution['status']
    path.write_text(json.dumps(result,indent=2));print({k:result[k] for k in ('status','cases','exact') if k in result});return execution['exit']

if __name__=='__main__':raise SystemExit(main())
