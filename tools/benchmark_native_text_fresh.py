"""Fresh-process native/DWrite constructor, first layout and memory matrix.

No output mismatch or unsupported input counts as a performance win. File-cache,
OS font-service and DLL-loader coldness are not controlled by this benchmark.
"""
import argparse
import datetime
import hashlib
import json
import os
import platform
import re
import statistics
import threading
from pathlib import Path
from verify_native_text import run_check, CheckFailure
from benchmark_native_text_performance import identity


def summarize(samples,cases):
    result=[]
    for index,name in cases:
        subset=[s for s in samples if s['case']==index];valid=bool(subset) and all(s.get('execution',{}).get('status')=='passed' and s.get('row',{}).get('exact') and s['row']['changed_ok'] and not s['row']['shape_error'] for s in subset)
        row=dict(case=index,name=name,status='exact' if valid else 'unsupported-or-different',samples=len(subset),providers={})
        for side,label in ((0,'native'),(1,'dwrite')):
            values=[s['row'] for s in subset if s['side']==side and 'row' in s]
            if not values:continue
            phases={}
            for phase,key in enumerate(('initialization','first_layout','changed_layout')):
                count=32 if phase==2 else 1
                times=[v['ticks'][phase]*1e6/v['frequency']/count for v in values]
                phases[key]=dict(median_us=statistics.median(times),min_us=min(times),max_us=max(times),median_cycles=statistics.median(v['cycles'][phase]/count for v in values))
            memory={}
            for phase,key in ((1,'initialized'),(2,'first_layout'),(3,'changed_layout'),(4,'released')):
                memory[key]=dict(private_delta=statistics.median(v['memory'][phase][0]-v['memory'][0][0] for v in values),working_set_delta=statistics.median(v['memory'][phase][1]-v['memory'][0][1] for v in values),peak_working_set=statistics.median(v['memory'][phase][2] for v in values),peak_private_commit=statistics.median(v['memory'][phase][3] for v in values))
            row['providers'][label]=dict(timings=phases,memory=memory)
        result.append(row)
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe',type=Path,default=Path('D:/MaiCord-build/Release/MaiCordNativeAppLineTests.exe'))
    parser.add_argument('--output-dir',type=Path,required=True)
    parser.add_argument('--repetitions',type=int,default=3)
    parser.add_argument('--case',type=int,action='append')
    args=parser.parse_args()
    if not 1<=args.repetitions<=20:parser.error('Use 1-20 bounded repetitions')
    root=Path(__file__).resolve().parents[1];source=root/'tests/native_fresh_engine_benchmark.inc';names=re.findall(r'\{"([a-z0-9-]+)",utf8_literal',source.read_text(encoding='utf-8'))
    selected=list(range(len(names))) if args.case is None else args.case
    if any(i<0 or i>=len(names) for i in selected) or len(set(selected))!=len(selected):parser.error('Unknown or duplicate case')
    cases=[(i,names[i]) for i in selected];out=args.output_dir.resolve();out.mkdir(parents=True,exist_ok=True);path=out/'fresh-engine.json'
    if path.exists():parser.error('Preserve existing measurements; choose an unused evidence directory')
    environment=os.environ.copy()
    environment['LOCALAPPDATA']=''
    binary=identity(args.exe);samples=[]
    report=dict(status='running',utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),windows=platform.platform(),executable=binary,source=identity(source),initialization_source=identity(root/'src/ui/text.cpp'),repetitions=args.repetitions,cases=cases,samples=samples,scope=__doc__)
    def save():path.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    save()
    for index,name in cases:
        for trial in range(args.repetitions):
            for order in range(2):
                side=order^((index+trial)&1);command=[str(args.exe.resolve()),'--fresh-engine',str(index),str(side)]
                try:execution=run_check(f'{index}-{trial}-{side}',command,root,out,2,threading.Event(),env=environment)
                except CheckFailure as error:execution=error.row
                raw=Path(execution['log']).read_text(encoding='utf-8');rows=[json.loads(line) for line in raw.splitlines() if line.startswith('{"kind":"fresh_engine"')]
                sample=dict(case=index,name=name,trial=trial,side=side,execution=execution,log=identity(Path(execution['log'])))
                if execution['exit']==0 and len(rows)==1:
                    value=rows[0]
                    if value['case']!=index or value['side']!=side or value['name']!=name or len(value['memory'])!=5:raise ValueError('Mismatched measurement record')
                    sample['row']=value
                samples.append(sample);save()
        print('completed',name,flush=True)
    if identity(args.exe)!=binary:raise ValueError('Binary changed during measurement')
    report['summary']=summarize(samples,cases);report['status']='completed' if all(r['status']=='exact' for r in report['summary']) else 'failed';save()
    print('Exact workloads',sum(r['status']=='exact' for r in report['summary']),'of',len(cases));print(path)
    return 0 if report['status']=='completed' else 1


if __name__=='__main__':raise SystemExit(main())
