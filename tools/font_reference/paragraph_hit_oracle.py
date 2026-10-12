"""Bounded adjacent-coordinate paragraph hit checks using the existing QA binary."""
from pathlib import Path
import argparse,hashlib,json,os,subprocess,time

def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native',type=Path,default=Path(__file__).resolve().parents[3]/'MaiCord-build/Release/MaiCordNativeAppLineTests.exe')
    parser.add_argument('--profile',choices=('myanmar','myanmar-tracking','tamil-tracking','tamil-direction','devanagari-direction','devanagari-tracking','arabic-context'),required=True)
    parser.add_argument('--sample',type=int,required=True)
    parser.add_argument('--family',default='Nirmala UI')
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--observations',type=Path,help='Also require every reference/geometry row to equal this earlier capture.')
    parser.add_argument('--observe-only',action='store_true',help='Collect references without native point assertions; never a passing verification.')
    args=parser.parse_args()
    if args.sample<0:parser.error('--sample must be nonnegative')
    args.output.parent.mkdir(parents=True,exist_ok=True);started=time.perf_counter()
    result=dict(status='running',profile=args.profile,sample=args.sample,family=args.family,native_sha256=digest(args.native))
    args.output.write_text(json.dumps(result)+'\n',encoding='utf-8')
    try:
        command=[str(args.native.resolve()),'--'+args.profile+'-profile','--family',args.family,'400']
        proc=subprocess.run(command,capture_output=True,timeout=2,creationflags=subprocess.CREATE_NO_WINDOW,
            env=dict(os.environ,MAICORD_HIT_OBSERVE='1' if args.observe_only else '2',MAICORD_DEVANAGARI_SAMPLE=str(args.sample)))
        if proc.returncode:raise RuntimeError(f'QA child exit {proc.returncode}: {proc.stdout[-2000:]!r}')
        rows=[json.loads(line) for line in proc.stdout.splitlines() if line.startswith(b'{')]
        kind='hit_observation_only' if args.observe_only else 'hit_boundary_verified'
        if not rows or any(row.get('kind')!=kind or not row.get('points') for row in rows):raise ValueError('Missing or unexpected observation records')
        if args.observations:
            saved=json.loads(args.observations.read_text(encoding='utf-8'))
            if saved['profile']!=args.profile or saved['sample']!=args.sample or saved.get('family','Nirmala UI')!=args.family:raise ValueError('Observation selection differs')
            stripped=lambda values:[{k:v for k,v in row.items() if k!='kind'} for row in values]
            if stripped(saved['layouts'])!=stripped(rows):raise ValueError('Reference or native geometry differs from the saved observation')
        result.update(status='observed_only' if args.observe_only else 'passed',layouts=rows,points=sum(len(row['points']) for row in rows),
            scope='Adjacent float point probes only; no pixel, performance or full editor acceptance.',observations=str(args.observations) if args.observations else None)
    except Exception as error:
        result.update(status='failed',error=str(error));raise
    finally:
        result['seconds']=time.perf_counter()-started
        args.output.write_text(json.dumps(result)+'\n',encoding='utf-8')
        print(json.dumps({k:v for k,v in result.items() if k!='layouts'}))
    return 0
if __name__=='__main__':raise SystemExit(main())
