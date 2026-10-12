"""Compare TSF ACP geometry through real text-store locks and native/DWrite editors.

Synthetic composition views exercise the COM adapter without a live IME service.
This verifies the editor/TSF boundary, not interactive candidate-window behavior.
"""
import argparse,json,os,re,threading
from pathlib import Path
from verify_native_text import run_check,CheckFailure
from benchmark_native_text_performance import identity

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir',type=Path,required=True)
    parser.add_argument('--bin-dir',type=Path,default=Path('D:/MaiCord-build/Release'))
    args=parser.parse_args();root=Path(__file__).resolve().parents[1];out=args.output_dir.resolve();out.mkdir(parents=True,exist_ok=True)
    destination=out/'tsf.json'
    if destination.exists():parser.error('Preserve previous evidence')
    report=dict(status='running',scope=__doc__,workers=[],sources=[identity(root/name) for name in ['tests/native_text_store_test.cpp','tests/tsf_composition_fixture.hpp','src/ui/native_text_script.hpp','src/ui/native_text_candidate.cpp','src/ui/native_text_styled.inc','src/text/lookup_plan.cpp','src/text/latin_run.cpp','src/text/latin_line.cpp','include/maicord/text/alphabetic_run.hpp']])
    lines=[]
    try:
        for provider in ['Native','DWrite']:
            exe=(args.bin_dir/('MaiCord'+provider+'TextStoreTests.exe')).resolve();before=identity(exe);data=out/provider;data.mkdir()
            env={k:v for k,v in os.environ.items() if not k.startswith('MAICORD_')};env['LOCALAPPDATA']=str(data)
            entry=dict(provider=provider,executable=before);report['workers'].append(entry)
            try:entry['execution']=run_check(provider,[str(exe),'--oracle-data'],root,out,10,threading.Event(),env=env)
            except CheckFailure as error:entry['execution']=error.row;raise
            raw=Path(entry['execution']['log']).read_text();values=raw.splitlines();assert identity(exe)==before
            tail=re.fullmatch(r'PASS (\d+) (\d+)',values[-1]);assert tail and int(tail[1])==44
            queries=[v for v in values if re.fullmatch(r'\d+(?:,\d+){8}',v)]
            assert len(queries)==int(tail[2]) and len([v for v in values if v.startswith('CASE ')])==220 and values.count('REJECT tab')==4
            entry.update(cases=44,phases=220,queries=len(queries),rejections=4,log=identity(Path(entry['execution']['log'])))
            lines.append(values)
        mismatch=next((i for i,(a,b) in enumerate(zip(*lines)) if a!=b),None)
        if mismatch is not None or len(lines[0])!=len(lines[1]):
            report['mismatch']=dict(line=mismatch,native=lines[0][mismatch] if mismatch is not None else len(lines[0]),reference=lines[1][mismatch] if mismatch is not None else len(lines[1]));raise ValueError('Native/DWrite TSF geometry differs')
        report['status']='passed'
    except Exception as error:report.update(status='failed',error=str(error));raise
    finally:destination.write_text(json.dumps(report,indent=2)+'\n')
    print('TSF parity:',report['workers'][0]['queries'],'queries;',destination)
if __name__=='__main__':main()
