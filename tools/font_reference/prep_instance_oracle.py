"""Verify retained native preparation policy against frozen reference probes."""
import argparse,json,re,subprocess
from pathlib import Path
from bold_geometry_oracle import identity

def run(root,source,exe):
    root.mkdir(parents=True,exist_ok=True);data=json.loads(source.read_text());rows=[]
    for index,case in enumerate(data['files']):
        font=Path(case['font']['path']);assert identity(font)==case['font'];glyph=1 if case['name'] in ['normal','instctrl-disabled','undefined-prep-call'] else 0
        p=subprocess.run([str(exe),str(font),'--simulate=1','--points',f'--instructions-state={glyph}'],capture_output=True,text=True,timeout=60,creationflags=0x08000000)
        log=root/f'{index}.txt';log.write_text(p.stdout+p.stderr);assert p.returncode in [0,6],(case['name'],p.returncode)
        def read(name):return int(re.search(r'^'+name+r'=(\d+)$',p.stdout,re.M)[1])
        assert read('instruction_prepare_status')==0 and read('instruction_control')==case['instruction_control']
        assert read('instruction_instance_prep_recovered')==int(case['missing_function'])==read('instruction_prepare_missing_function')
        rows.append(dict(name=case['name'],recovered=case['missing_function'],instance_bytes=read('instruction_instance_bytes'),font=case['font'],log=identity(log)))
    result=dict(native=identity(exe),reference=identity(source),cases=rows);(root/'observations.json').write_text(json.dumps(result,indent=2));print(len(rows),'retained preparation policies exact')

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('--reference',required=True,type=Path);p.add_argument('--exe',required=True,type=Path);a=p.parse_args();run(a.root.resolve(),a.reference.resolve(),a.exe.resolve())
