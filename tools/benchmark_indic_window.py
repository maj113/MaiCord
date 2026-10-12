"""Bounded before/after measurements of warm Bengali/Gujarati native runs."""
import argparse,hashlib,json,os,statistics,threading,time
from pathlib import Path
from verify_native_text import run_check

def identity(path):
    return dict(path=str(path),bytes=path.stat().st_size,sha256=hashlib.sha256(path.read_bytes()).hexdigest())

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--side',choices=('baseline','candidate','compare'),required=True)
    parser.add_argument('--output-dir',type=Path,required=True)
    parser.add_argument('--font',type=Path,required=True)
    args=parser.parse_args();root=Path(__file__).resolve().parents[1];out=args.output_dir.resolve();out.mkdir(parents=True,exist_ok=True)
    target=out/(args.side+'.json');started=time.perf_counter()
    if args.side=='baseline' and target.exists():parser.error('Keep the existing baseline; choose a new evidence directory for a different experiment')
    result=dict(status='running',side=args.side)
    target.write_text(json.dumps(result),encoding='utf-8')
    if args.side!='compare':(out/'compare.json').write_text(json.dumps(dict(status='incomplete',pending_side=args.side)),encoding='utf-8')
    try:
        if args.side=='compare':
            sides={s:json.loads((out/(s+'.json')).read_text(encoding='utf-8')) for s in ('baseline','candidate')}
            if any(r['status']!='measured' for r in sides.values()):raise ValueError('Two successful measurements are required')
            if any(sides['baseline'][k]!=sides['candidate'][k] for k in ('font','fixture','entry')):raise ValueError('Measurement inputs differ')
            rows=[]
            if len(sides['baseline']['rows'])!=len(sides['candidate']['rows']):raise ValueError('Different corpus counts')
            for before,after in zip(sides['baseline']['rows'],sides['candidate']['rows']):
                if any(before[k]!=after[k] for k in ('script','corpus','bytes','units','glyphs','hash','iterations')):raise ValueError('Different measured work or output fingerprint')
                cycles=[statistics.median(b[1] for b in r['batches'])/r['iterations'] for r in (before,after)]
                microseconds=[statistics.median(b[0] for b in r['batches'])/r['frequency']/r['iterations']*1e6 for r in (before,after)]
                rows.append(dict(script=before['script'],corpus=before['corpus'],cycles=cycles,cycle_ratio=cycles[1]/cycles[0],microseconds=microseconds,time_ratio=microseconds[1]/microseconds[0]))
            result.update(status='compared',rows=rows,sides=sides)
            for row in rows:print(row['script'],row['corpus'],'cycles',round(row['cycle_ratio'],3),'time',round(row['time_ratio'],3))
        else:
            executable=root.parent/'MaiCord-build/Release/MaiCordDevanagariPlanTests.exe'
            font=args.font.resolve()
            result.update(executable=identity(executable),font=identity(font),fixture=identity(root/'tests/indic_run_bench.inc'),entry=identity(root/'tests/devanagari_plan_test.cpp'),source=identity(root/'src/text/bengali_shape_run.cpp'))
            if args.side=='baseline':(out/'coordinator-before.cpp').write_bytes((root/'src/text/bengali_shape_run.cpp').read_bytes())
            rows=[];steps=[];stop=threading.Event()
            for script in ('bengali','gujarati'):
                env={k:v for k,v in os.environ.items() if not k.startswith('MAICORD_')}
                env.update(MAICORD_INDIC_RUN_BENCH='1');env['MAICORD_'+script.upper()+'_RUN']='1'
                step=run_check(args.side+'-'+script,[str(executable),str(font)],root,out,2,stop,env=env);steps.append(step)
                measured=[json.loads(line) for line in Path(step['log']).read_text(encoding='utf-8').splitlines() if line.startswith('{')]
                if len(measured)!=6 or any(r['script']!=script or len(r['batches'])!=7 for r in measured):raise ValueError('Incomplete measurements')
                rows.extend(measured)
            if identity(executable)!=result['executable']:raise ValueError('Executable changed during measurement')
            result.update(status='measured',rows=rows,steps=steps)
    except Exception as error:
        result.update(status='failed',error=str(error));raise
    finally:
        result['seconds']=time.perf_counter()-started;target.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
        print(args.side,result['status'],round(result['seconds'],2),'seconds;',target)
    return 0
if __name__=='__main__':raise SystemExit(main())
