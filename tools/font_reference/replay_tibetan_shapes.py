"""Replay accepted Tibetan shape reports in one bounded worker; no new observations."""
import argparse,json,os,struct,subprocess,sys,threading,time
from pathlib import Path
from tamil_shape_oracle import decode
from tamil_cluster_oracle import identity

def worker(args):
    start=time.perf_counter();native=identity(args.native);reports=[];total=0
    for index,path in enumerate(args.report):
        saved=json.loads(path.read_text(encoding='utf-8'))
        if saved['status']!='passed':raise ValueError('Only accepted reports may serve as replay expectations')
        inputs=saved['inputs']
        for item in inputs.values():
            if isinstance(item,dict) and identity(Path(item['path']))!=item:raise ValueError('Changed input identity')
        cases=[(tuple(r['scalars']),'saved') for r in saved['rows']]
        payload=bytearray(b'MDSHP1\0\0'+struct.pack('<I',len(cases)))
        for s,_ in cases:payload+=struct.pack('<I',len(s))+struct.pack('<'+'I'*len(s),*s)
        run=subprocess.run([str(args.native)],input=payload,capture_output=True,timeout=2,
            env=dict(os.environ,MAICORD_TIBETAN_FONT=inputs['font']['path'],MAICORD_TIBETAN_INDEPENDENT=str(int(inputs['independent'])),MAICORD_TIBETAN_RTL=str(int(inputs['rtl'])),
                MAICORD_TIBETAN_OWNED=str(int(args.owned or args.compact or saved.get('owned',False))),MAICORD_TIBETAN_COMPACT=str(int(args.compact or saved.get('compact',False)))),creationflags=subprocess.CREATE_NO_WINDOW)
        if run.returncode:raise RuntimeError(repr(run.stdout[-500:])+repr(run.stderr))
        raw=args.output_dir/f'{index}-native.bin';raw.write_bytes(run.stdout)
        actual=decode(run.stdout,cases,True);differences=[];missing_rejections=0
        for n,(a,row) in enumerate(zip(actual,saved['rows'])):
            b=row['reference']
            if (args.compact or saved.get('compact',False)) and 0 in b['glyphs']:
                if a['status']==8 and not a['glyphs'] and not a['placements']:missing_rejections+=1
                else:differences.append(dict(index=n,native=a,reference=b,scalars=row['scalars'],expected='missing-glyph rejection'))
                continue
            if a['status'] or any(a[k]!=b[k] for k in ('glyphs','clusters','placements')):differences.append(dict(index=n,native=a,reference=b,scalars=row['scalars']))
        total+=len(cases);reports.append(dict(report=identity(path),native_output=identity(raw),cases=len(cases),exact=len(cases)-len(differences)-missing_rejections,missing_rejections=missing_rejections,differences=differences))
    if identity(args.native)!=native:raise ValueError('Native executable changed')
    result=dict(status='passed' if reports and all(not r['differences'] for r in reports) else 'failed',cases=total,exact=sum(r['exact'] for r in reports),missing_rejections=sum(r['missing_rejections'] for r in reports),seconds=time.perf_counter()-start,native=native,source=identity(Path(__file__)),scope=__doc__,force_owned=args.owned,force_compact=args.compact,reports=reports)
    (args.output_dir/'result.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');print(result['exact'],'/',total,'exact clusters');return int(result['status']!='passed')

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--native',required=True,type=Path)
    p.add_argument('--report',required=True,action='append',type=Path);p.add_argument('--output-dir',required=True,type=Path)
    p.add_argument('--owned',action='store_true',help='Replay through retained font admission and UTF-8 shaping')
    p.add_argument('--compact',action='store_true',help='Replay through retained compact publication')
    p.add_argument('--worker',action='store_true',help=argparse.SUPPRESS);args=p.parse_args()
    if args.worker:return worker(args)
    args.output_dir.mkdir(parents=True,exist_ok=True);path=args.output_dir/'result.json'
    if path.exists():p.error('Preserve existing evidence')
    path.write_text('{"status":"running"}')
    sys.path.insert(0,str(Path(__file__).resolve().parents[1]));from verify_native_text import run_check,CheckFailure
    try:r=run_check('tibetan-saved-shapes',[sys.executable,str(Path(__file__).resolve()),*sys.argv[1:],'--worker'],Path.cwd(),args.output_dir,5,threading.Event())
    except CheckFailure as error:r=error.row
    result=json.loads(path.read_text());result['execution']=r
    if r['exit']:result['status']=r['status']
    path.write_text(json.dumps(result,indent=2)+'\n');return r['exit']
if __name__=='__main__':raise SystemExit(main())
