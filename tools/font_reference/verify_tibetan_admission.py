"""Replay a bounded authored admission batch through retained native plans."""
import argparse,json,sys,threading,time
from pathlib import Path
from types import SimpleNamespace
from tamil_cluster_oracle import identity
from verify_tibetan_shape import worker as shape_worker

def worker(args):
    started=time.perf_counter();saved=json.loads(args.observations.read_text());rows=[]
    reference=Path(saved['reference']['path']);assert saved['reference']['sha256']==identity(reference)['sha256']
    cases=args.output_dir/'cases.json';cases.write_text(json.dumps(saved['cases']))
    for index,font in enumerate(saved['fonts']):
        source=Path(font['font']['path']);assert font['font']['sha256']==identity(source)['sha256']
        for mode in font['observations']:
            independent=mode['independent'];name=f'font-{index}-{int(independent)}';obs=args.output_dir/(name+'-observations.json')
            inputs=dict(font=identity(source),reference=identity(reference),cases=identity(cases),independent=independent,rtl=False,run=True)
            obs.write_text(json.dumps(dict(inputs=inputs,rows=mode['rows'],derived_from=identity(args.observations))))
            folder=args.output_dir/name;folder.mkdir()
            code=shape_worker(SimpleNamespace(font=source,reference=reference,native=args.native,case_file=cases,output_dir=folder,
                observations=obs,independent=independent,rtl=False,run=True,owned=True,compact=args.compact))
            result=json.loads((folder/'result.json').read_text());rows.append(dict(font=source.name,independent=independent,
                cases=result['cases'],exact=result['exact'],missing_rejections=result.get('missing_rejections',0),exit=code,report=identity(folder/'result.json')))
    result=dict(status='passed' if all(not r['exit'] for r in rows) else 'failed',cases=sum(r['cases'] for r in rows),
        exact=sum(r['exact'] for r in rows),missing_rejections=sum(r['missing_rejections'] for r in rows),seconds=time.perf_counter()-started,observations=identity(args.observations),
        native=identity(args.native),compact=args.compact,rows=rows)
    (args.output_dir/'result.json').write_text(json.dumps(result));return 0 if result['status']=='passed' else 1

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for key in ('native','observations','output-dir'):p.add_argument('--'+key,type=Path,required=True)
    p.add_argument('--compact',action='store_true');p.add_argument('--worker',action='store_true',help=argparse.SUPPRESS);args=p.parse_args()
    for key in ('native','observations','output_dir'):setattr(args,key,getattr(args,key).resolve())
    if args.worker:return worker(args)
    if args.output_dir.exists() and any(args.output_dir.iterdir()):p.error('Preserve existing evidence')
    args.output_dir.mkdir(parents=True,exist_ok=True);path=args.output_dir/'result.json';path.write_text('{"status":"running"}')
    sys.path.insert(0,str(Path(__file__).resolve().parents[1]));from verify_native_text import run_check,CheckFailure
    try:execution=run_check('tibetan-admission-check',[sys.executable,str(Path(__file__).resolve()),*sys.argv[1:],'--worker'],Path.cwd(),args.output_dir,5,threading.Event())
    except CheckFailure as e:execution=e.row
    result=json.loads(path.read_text());result['execution']=execution
    if execution['exit']:result['status']=execution['status']
    path.write_text(json.dumps(result));print({k:result[k] for k in ('status','cases','exact') if k in result});return execution['exit']

if __name__=='__main__':raise SystemExit(main())
