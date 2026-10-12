"""Replay saved Windows Khmer source boundaries in one native process.

Deduplicate scalar inputs for execution, then compare every font/corpus record.
This verifies source boundaries only, not glyph shaping, placement or pixels.
"""
import argparse,json,sys,time,threading
from pathlib import Path
from gsub_execution_oracle import identity
from khmer_expansion_oracle import native_stream

def worker(args):
    start=time.perf_counter();unique={};sources=[];records=[];validated={}
    for path in args.observations:
        report=json.loads(path.read_text(encoding='utf-8'))
        for item in report['inputs']:
            p=Path(item['path'])
            if str(p) not in validated:validated[str(p)]=identity(p)
            if validated[str(p)]['sha256']!=item['sha256']:raise RuntimeError('Changed reference/font input')
        source=identity(path);sources.append(source)
        for row in report['rows']:
            text=tuple(row['scalars']);expected=row.get('expected')
            if not text or len(text)>256 or not expected or len(expected)!=len(text) or expected[0]!=1 or any(x not in (0,1) for x in expected):raise RuntimeError('Incomplete or invalid expected boundaries')
            index=unique.setdefault(text,len(unique));records.append((index,expected,str(path),row.get('group')))
    cases=[(s,'saved') for s in unique];data=native_stream(args.native,cases);at=0;actual=[]
    for s,g in cases:actual.append(list(data[at:at+len(s)]));at+=len(s)
    if at!=len(data):raise RuntimeError('Wrong native stream length')
    differences=[]
    for index,expected,source,group in records:
        if actual[index]!=expected:differences.append(dict(scalars=cases[index][0],expected=expected,native=actual[index],source=source,group=group))
    result=dict(status='failed' if differences else 'passed',cases=len(records),unique_inputs=len(cases),exact=len(records)-len(differences),
        seconds=time.perf_counter()-start,scope=__doc__,native=identity(args.native),observations=sources,inputs=list(validated.values()),differences=differences)
    (args.output_dir/'result.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(result['exact'],'/',len(records),'comparisons;',len(cases),'distinct inputs;',round(result['seconds'],2),'seconds')
    return int(bool(differences))

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native',required=True,type=Path);p.add_argument('--observations',required=True,type=Path,action='append');p.add_argument('--output-dir',required=True,type=Path)
    p.add_argument('--worker',action='store_true',help=argparse.SUPPRESS);args=p.parse_args()
    if args.worker:return worker(args)
    args.output_dir.mkdir(parents=True,exist_ok=True);path=args.output_dir/'result.json'
    if any(p.resolve()==path.resolve() for p in args.observations):p.error('Preserve reference observations')
    path.write_text('{"status":"running"}',encoding='utf-8')
    sys.path.insert(0,str(Path(__file__).resolve().parents[1]));from verify_native_text import run_check,CheckFailure
    try:execution=run_check('khmer-boundary-replay',[sys.executable,str(Path(__file__).resolve()),*sys.argv[1:],'--worker'],Path.cwd(),args.output_dir,5,threading.Event())
    except CheckFailure as error:execution=error.row
    result=json.loads(path.read_text(encoding='utf-8'));result['execution']=execution
    if execution['exit']:result['status']=execution['status']
    path.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');return execution['exit']
if __name__=='__main__':raise SystemExit(main())
