"""Run the native-only application integration probe with a five-second deadline."""
from pathlib import Path
import argparse,json,os,threading,time
from verify_native_text import run_check,CheckFailure
from verify_hangul_render import identity

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe',type=Path,required=True)
    parser.add_argument('--output-dir',type=Path,required=True)
    args=parser.parse_args();root=Path(__file__).resolve().parents[1]
    out=args.output_dir.resolve();out.mkdir(parents=True,exist_ok=True)
    data=out/'data';data.mkdir(exist_ok=True);log=data/'MaiCord/logs/maicord.log'
    offset=log.stat().st_size if log.exists() else 0;started=time.perf_counter()
    before=identity(args.exe.resolve());result=dict(status='running',executable=before,log_offset=offset)
    target=out/'app-result.json';target.write_text(json.dumps(result),encoding='utf-8')
    env={k:v for k,v in os.environ.items() if not k.startswith('MAICORD_')}
    env.update(MAICORD_NATIVE_BOOTSTRAP_PROBE='1',MAICORD_UI_PROBE_ONCE='1',MAICORD_GPU_METRICS='1',LOCALAPPDATA=str(data))
    try:
        result['process']=run_check('app-bootstrap',[str(args.exe.resolve())],root,out,5,threading.Event(),env=env)
        text=log.read_bytes()[offset:].decode('utf-8')
        required=('native_renderer_bootstrap_passed','application_text_form_passed','renderer_font_alias_churn_simulations_and_foreign_session_passed','native_application_text_objects_absent_passed','application_renderer_release_passed')
        if any(marker not in text for marker in required) or 'session_has_live_handles' in text:raise RuntimeError('Incomplete verification or leaked ownership')
        if identity(args.exe.resolve())!=before:raise RuntimeError('Executable changed during verification')
        result['status']='passed'
    except CheckFailure as error:result.update(status='failed',process=error.row,error=str(error))
    except Exception as error:result.update(status='failed',error=str(error))
    finally:
        result['seconds']=time.perf_counter()-started
        result['log_excerpt']=log.read_bytes()[offset:].decode('utf-8') if log.exists() else ''
        target.write_text(json.dumps(result,indent=2),encoding='utf-8')
        print(result['status'],round(result['seconds'],2),'seconds; evidence',target)
    return 0 if result['status']=='passed' else 1

if __name__=='__main__':raise SystemExit(main())
