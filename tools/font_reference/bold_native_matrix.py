"""Compare complete native bold/bold-oblique outlines with Windows."""
import argparse,json,re,subprocess
from pathlib import Path
from bold_geometry_oracle import identity

def run(root,source,exe,renderer=False):
    root.mkdir(parents=True,exist_ok=True);data=json.loads(source.read_text());files=[]
    for item in data['files']:
        font=Path(item['font']['path']);assert identity(font)==item['font'];modes={}
        for mode in [1,3]:
            args=[str(exe),str(font),f'--simulate={mode}']
            if renderer:args+=['--renderer']
            args+=['--instructions']
            p=subprocess.run(args,capture_output=True,text=True,timeout=120,creationflags=0x08000000)
            log=root/f'{font.stem}-{mode}.txt';log.write_text(p.stdout+p.stderr)
            def read(name):
                m=re.search(r'^'+name+r'=(\d+)$',p.stdout,re.M);return int(m[1]) if m else None
            result=dict(exit_code=p.returncode,compared=read('outlines_compared'),failures=read('outline_failures'),differences=read('outline_differences'),log=identity(log))
            modes[str(mode)]=result;print(item['family'],mode,result['compared'],result['failures'],result['differences'],p.returncode,flush=True)
        files.append(dict(family=item['family'],font=item['font'],modes=modes))
    result=dict(native=identity(exe),inventory=identity(source),renderer=renderer,files=files)
    (root/'observations.json').write_text(json.dumps(result,indent=2))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('--source',type=Path,required=True);p.add_argument('--exe',type=Path,required=True);p.add_argument('--renderer',action='store_true');a=p.parse_args();run(a.root.resolve(),a.source.resolve(),a.exe.resolve(),a.renderer)
