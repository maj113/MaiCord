"""Bounded, sequential native/DirectWrite warm paragraph measurements."""
import argparse
import datetime
import hashlib
import json
import platform
import statistics
import subprocess
import threading
from pathlib import Path
from verify_native_text import run_check


def identity(path):
    raw=path.read_bytes()
    return dict(path=str(path.resolve()),bytes=len(raw),sha256=hashlib.sha256(raw).hexdigest())


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe',type=Path,default=Path('D:/MaiCord-build/Release/MaiCordNativeAppLineTests.exe'))
    parser.add_argument('--output-dir',type=Path,required=True)
    args=parser.parse_args();root=Path(__file__).resolve().parents[1];out=args.output_dir.resolve();out.mkdir(parents=True,exist_ok=True)
    result=out/'warm-paragraphs.json'
    if result.exists():parser.error('Preserve existing measurements; choose an unused evidence directory')
    import winreg
    with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,r'HARDWARE\DESCRIPTION\System\CentralProcessor\0') as key:
        cpu=winreg.QueryValueEx(key,'ProcessorNameString')[0].strip()
    power=subprocess.run(['powercfg','/getactivescheme'],capture_output=True,text=True,timeout=2,creationflags=subprocess.CREATE_NO_WINDOW)
    power.check_returncode();exe=args.exe.resolve();binary=identity(exe);rows=[];executions=[]
    for script in ('odia','telugu','kannada'):
        execution=run_check(script+'-warm',[str(exe),'--'+script+'-bench'],root,out,5,threading.Event());executions.append(execution)
        raw=Path(execution['log']).read_text(encoding='utf-8')
        batches=[json.loads(line) for line in raw.splitlines() if line.startswith('{') and 'paragraph_benchmark' in line]
        if len(batches)!=4 or sorted(b['mode'] for b in batches)!=[0,1,2,3]:raise ValueError('Missing benchmark modes')
        for row in batches:
            if len(row['batches'])!=10 or len(row['cycle_batches'])!=10 or row['iterations']!=128 or row['frequency']<=0:raise ValueError('Unexpected measurement dimensions')
            scale=1e6/row['frequency']/row['iterations']
            rows.append(dict(script=script,mode=row['mode'],native_us=statistics.median(p[0] for p in row['batches'])*scale,dwrite_us=statistics.median(p[1] for p in row['batches'])*scale,native_cycles=statistics.median(p[0] for p in row['cycle_batches'])/row['iterations'],dwrite_cycles=statistics.median(p[1] for p in row['cycle_batches'])/row['iterations'],batches=10,calls_per_batch=128,log=identity(Path(execution['log']))))
    if identity(exe)!=binary:raise ValueError('Binary changed during measurement')
    source=identity(root/'tests/styled_paragraph_benchmark.inc')
    result.write_text(json.dumps(dict(status='passed',measured_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),executable=binary,source=source,host=dict(cpu=cpu,windows=platform.platform(),power=power.stdout.strip()),executions=executions,results=rows,scope='Warm paired layout calls only. Output checked before timing. Sequential processes; alternating providers. Excludes initialization, process memory, GPU, end-to-end latency and throughput.'),indent=2)+'\n',encoding='utf-8')
    print(result)
    for row in rows:print(row['script'],row['mode'],round(row['native_us'],2),round(row['dwrite_us'],2))


if __name__=='__main__':main()
