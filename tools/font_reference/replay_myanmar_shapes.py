"""Replay saved Myanmar shape reports without starting Python for every font.

One five-second process-tree deadline; each native child retains its two-second
deadline. Fonts, observer, cases and modes must still match saved observations.
The report list is explicit, never a silently reduced sample.
"""
import argparse,json,sys,threading,time
from pathlib import Path
from types import SimpleNamespace
from verify_myanmar_shape import worker
from tamil_cluster_oracle import identity

def replay(args):
    start=time.perf_counter();reports=json.loads(args.reports.read_text());rows=[]
    if not reports:raise ValueError('Empty replay list')
    for i,name in enumerate(reports):
        path=Path(name).resolve();saved=json.loads(path.read_text())
        if saved['status']!='passed':raise ValueError('Replay requires a complete accepted source report')
        if (args.owned or args.compact) and saved['scope']=='Myanmar single cluster':raise ValueError('Run adapter override requires run observations')
        observations=path.parent/'observations.json'
        if not observations.exists():observations=Path(saved['observations']['path'])
        values=saved['inputs'];out=args.output_dir/f'row-{i}';out.mkdir()
        options=SimpleNamespace(font=Path(values['font']['path']),reference=Path(values['reference']['path']),
            case_file=Path(values['cases']['path']),native=args.native,output_dir=out,observations=observations,
            independent=values['independent'],rtl=values['rtl'],run=saved['scope']=='Myanmar full scalar run',
            owned=args.owned or args.compact or saved['scope'].startswith('Myanmar owned'),compact=args.compact or saved['scope'].endswith('compact'),failures_only=True)
        code=worker(options);result_path=out/'result.json';result=json.loads(result_path.read_text())
        result['replayed_from']=identity(path);result_path.write_text(json.dumps(result))
        rows.append(dict(source=identity(path),result=identity(result_path),cases=result['cases'],exact=result['exact'],missing_rejections=result.get('missing_rejections',0),exit=code))
    result=dict(status='passed' if all(not r['exit'] for r in rows) else 'failed',reports=len(rows),
        cases=sum(r['cases'] for r in rows),exact=sum(r['exact'] for r in rows),missing_rejections=sum(r['missing_rejections'] for r in rows),seconds=time.perf_counter()-start,
        native=identity(args.native),report_list=identity(args.reports),rows=rows)
    (args.output_dir/'result.json').write_text(json.dumps(result))
    return 0 if result['status']=='passed' else 1

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for key in ('native','reports','output-dir'):p.add_argument('--'+key,type=Path,required=True)
    p.add_argument('--owned',action='store_true');p.add_argument('--compact',action='store_true')
    p.add_argument('--worker',action='store_true',help=argparse.SUPPRESS);args=p.parse_args()
    for key in ('native','reports','output_dir'):setattr(args,key,getattr(args,key).resolve())
    if args.worker:return replay(args)
    if args.output_dir.exists() and any(args.output_dir.iterdir()):p.error('Preserve existing evidence')
    args.output_dir.mkdir(parents=True,exist_ok=True);path=args.output_dir/'result.json';path.write_text('{"status":"running"}')
    sys.path.insert(0,str(Path(__file__).resolve().parents[1]));from verify_native_text import run_check,CheckFailure
    try:execution=run_check('myanmar-replay',[sys.executable,str(Path(__file__).resolve()),*sys.argv[1:],'--worker'],Path.cwd(),args.output_dir,5,threading.Event())
    except CheckFailure as error:execution=error.row
    result=json.loads(path.read_text());result['execution']=execution
    if execution['exit']:result['status']=execution['status']
    path.write_text(json.dumps(result));print({k:result[k] for k in ('status','reports','cases','exact') if k in result})
    return execution['exit']

if __name__=='__main__':raise SystemExit(main())
