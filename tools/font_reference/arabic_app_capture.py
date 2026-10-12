"""Run baseline and mark-ordering paths from the same native executable."""
import argparse,json,subprocess
from pathlib import Path
from gsub_execution_oracle import identity
from arabic_app_compare import compare

def run(root,exe,name):
    reference=root/'reference/observations.json';corpus=root/'reference/corpus.bin';runs=[]
    for mode in ['baseline','ordered']:
        command=[str(exe),str(corpus)]+(['--original-order'] if mode=='baseline' else [])
        p=subprocess.run(command,capture_output=True,text=True,timeout=120,creationflags=0x08000000)
        log=root/f'{name}-{mode}.jsonl';log.write_text(p.stdout);assert p.returncode==0,(command,p.returncode,p.stderr)
        output=root/f'{name}-{mode}-comparison.json';compare(reference,log,output)
        runs.append(dict(mode=mode,command=command,exit_code=p.returncode,log=identity(log),comparison=identity(output)))
    (root/f'{name}-capture.json').write_text(json.dumps(dict(executable=identity(exe),corpus=identity(corpus),reference=identity(reference),runs=runs),indent=2))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('--exe',type=Path,required=True);p.add_argument('--name',required=True);a=p.parse_args();run(a.root.resolve(),a.exe.resolve(),a.name)
