"""Capture either build in place, then compare cached Indic app images."""
from pathlib import Path
import argparse,json,os,re,threading,time
from verify_native_text import run_check
from verify_hangul_render import identity,bmp_difference

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--script',choices=('thai','tamil','bengali','gujarati','gurmukhi','odia','telugu','kannada','malayalam','sinhala','khmer','myanmar','tibetan'),default='tamil')
    parser.add_argument('--side',choices=('native','reference','compare'),required=True)
    parser.add_argument('--exe',type=Path)
    parser.add_argument('--wrapped',action='store_true',help='Use six wrapped profile biographies instead of four field strings.')
    parser.add_argument('--output-dir',type=Path,required=True)
    parser.add_argument('--reference-dir',type=Path,help='Compare against an existing verified reference capture without rebuilding or copying its executable.')
    parser.add_argument('--native-bootstrap',action='store_true',help='Create the native renderer without Windows text objects.')
    args=parser.parse_args();root=Path(__file__).resolve().parents[1];out=args.output_dir.resolve();out.mkdir(parents=True,exist_ok=True)
    if args.native_bootstrap and args.side!='native':parser.error('--native-bootstrap requires --side native')
    if args.reference_dir and args.side!='compare':parser.error('--reference-dir requires --side compare')
    prefix=args.script+('-wrap' if args.wrapped else '')
    names={f'{prefix}-{sample}-{mode}-{scale}.bmp' for sample in range(6 if args.wrapped else 4) for mode in range(3) for scale in range(2)}
    started=time.perf_counter();result=dict(status='running',side=args.side,wrapped=args.wrapped,script=args.script,native_bootstrap=args.native_bootstrap)
    target=out/('result.json' if args.side=='compare' else args.side+'.json')
    target.write_text(json.dumps(result)+'\n',encoding='utf-8')
    if args.side!='compare':(out/'result.json').write_text(json.dumps(dict(status='incomplete',pending_side=args.side))+'\n',encoding='utf-8')
    try:
        if args.side!='compare':
            if not args.exe:parser.error('--exe is required when capturing')
            executable=args.exe.resolve();before=identity(executable)
            captures=out/(args.side+'-captures');data=out/(args.side+'-data');captures.mkdir(exist_ok=True);data.mkdir(exist_ok=True)
            prior_times={p.name:p.stat().st_mtime_ns for p in captures.glob('*.bmp')}
            log=data/'MaiCord/logs/maicord.log';log_offset=log.stat().st_size if log.exists() else 0
            env={k:v for k,v in os.environ.items() if not(k.startswith('MAICORD_') and k.endswith('_RENDER_PROBE'))}
            env['MAICORD_'+args.script.upper()+('_WRAP' if args.wrapped else '')+'_RENDER_PROBE']='1'
            env.update(MAICORD_GPU_METRICS='1',MAICORD_UI_PROBE_CAPTURE_DIR=str(captures),LOCALAPPDATA=str(data))
            if args.native_bootstrap:env['MAICORD_NATIVE_BOOTSTRAP_PROBE']='1'
            else:env.pop('MAICORD_NATIVE_BOOTSTRAP_PROBE',None)
            process=run_check(args.side,[str(executable)],root,out,5,threading.Event(),env=env)
            if identity(executable)!=before:raise RuntimeError('Executable changed during capture')
            text=log.read_bytes()[log_offset:].decode('utf-8')
            if args.native_bootstrap and 'native_renderer_bootstrap_passed' not in text:raise RuntimeError('Native-only bootstrap missing')
            if prefix.replace('-','_')+'_render_probe_passed' not in text or 'session_has_live_handles' in text:raise RuntimeError('Missing successful capture or leaked font ownership')
            if ('native_outline_decodes=' in text)!=(args.side=='native'):raise RuntimeError('Wrong renderer configuration')
            counters={k:int(v) for k,v in re.findall(r'\b(native_\w+)=(\d+)(?=\r?$)',text,re.MULTILINE)}
            if args.side=='native' and (counters.get('native_outline_failures')!=0 or counters.get('native_colr_builds',0)<2 or counters.get('native_paragraphs_shaped',0)==0):raise RuntimeError('Native outline, COLRv1 or paragraph evidence missing')
            if {p.name for p in captures.glob('*.bmp')}!=names:raise RuntimeError('Incomplete capture set')
            if any(prior_times.get(name)==(captures/name).stat().st_mtime_ns for name in names):raise RuntimeError('A capture was not refreshed by this run')
            result.update(status='captured',executable=before,process=process,log=identity(log),log_offset=log_offset,fresh_captures=True,counters=counters,
                source=identity(root/'tests/integration/ui_probe.cpp'),fixture=identity(root/'tests/integration/thai_wrapped_probe.inc') if args.wrapped else None,captures={name:identity(captures/name) for name in sorted(names)})
        else:
            directories={'native':out,'reference':args.reference_dir.resolve() if args.reference_dir else out}
            sides={side:json.loads((directories[side]/(side+'.json')).read_text()) for side in ('native','reference')}
            if any(r['status']!='captured' or r.get('wrapped',False)!=args.wrapped or r.get('script','tamil')!=args.script for r in sides.values()):raise RuntimeError('Both successful side captures are required')
            if sides['native']['source']!=sides['reference']['source'] or sides['native'].get('fixture')!=sides['reference'].get('fixture'):raise RuntimeError('Capture fixture versions differ')
            comparisons=[]
            for name in sorted(names):
                images={side:identity(directories[side]/(side+'-captures')/name) for side in sides}
                if any(images[side]!=sides[side]['captures'][name] for side in sides):raise RuntimeError('Captured image changed')
                exact=images['native']['sha256']==images['reference']['sha256']
                comparisons.append(dict(name=name,exact=exact,difference=None if exact else bmp_difference(Path(images['native']['path']),Path(images['reference']['path']))))
            result.update(status='passed' if all(c['exact'] for c in comparisons) else 'different',comparisons=comparisons,sides=sides,
                reference_manifest=identity(directories['reference']/'reference.json'),reference_reused=bool(args.reference_dir),
                scope=f'Six wrapped {args.script.title()} profile biographies; 144/216/360-DIP viewports, 2/4/6 repetitions, 96/144 DPI. No whole-app resource acceptance.' if args.wrapped else f'Four {args.script.title()} app-field strings; plain, selection, composition; 96/144 DPI, tracking and bidi variants. No whole-app resource acceptance.')
    except Exception as error:
        result.update(status='failed',error=str(error));raise
    finally:
        result['seconds']=time.perf_counter()-started;target.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
        print(args.side,result['status'],round(result['seconds'],2),'seconds; evidence',target)
    return 0 if result['status'] in ('captured','passed') else 1
if __name__=='__main__':raise SystemExit(main())
