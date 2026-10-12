"""Compare native oblique outlines in files observed by the multilingual app audit.

Every file face is tested at its default axes; required Bahnschrift weights are
also tested explicitly. Synthetic-bold widening is a separate pending provider.
CFF metrics compare the unsimulated charstring box; output ink bounds are oblique.
"""
from pathlib import Path
import argparse,hashlib,json,re,subprocess
p=argparse.ArgumentParser(description=__doc__);p.add_argument('root',type=Path);p.add_argument('--inventory',type=Path,required=True);p.add_argument('--config',default='Release',choices=['Release']);p.add_argument('--renderer',action='store_true');a=p.parse_args()
inventory=json.loads(a.inventory.read_text(encoding='utf-8'));name=f'oblique-{a.config.lower()}'+('-renderer' if a.renderer else '');folder=a.root/name;folder.mkdir(exist_ok=True)
exe=a.root/f'MaiCordFontReference.exe';rows=[]
for index,font in enumerate(inventory['files']):
 path=Path(font['path']);assert hashlib.sha256(path.read_bytes()).hexdigest()==font['sha256']
 for variation in ([0,400,600] if path.stem.lower()=='bahnschrift' else [0]):
  options=['--simulate=2','--instructed',f'--gvar{variation}'] if variation else ['--simulate=2','--instructions']
  if a.renderer:options.insert(1,'--renderer')
  result=subprocess.run([str(exe),str(path),*options],capture_output=True,text=True,timeout=60)
  (folder/f'{index:02d}-{path.stem}-{variation}.txt').write_text(result.stdout+result.stderr)
  metrics={k:int(v) for k,v in re.findall(r'^(\w+)=(\d+)$',result.stdout,re.M)}
  rows.append(dict(font=font,weight=variation,exit=result.returncode,metrics=metrics));print(path.name,variation,result.returncode,metrics.get('outlines_compared'),metrics.get('outline_differences'),flush=True)
result=dict(scope=__doc__,executable=dict(path=str(exe),sha256=hashlib.sha256(exe.read_bytes()).hexdigest()),files=rows)
(a.root/f'{name}.json').write_text(json.dumps(result,indent=2))
print('instances',len(rows),'failed',sum(bool(r['exit']) for r in rows),'outlines',sum(r['metrics'].get('outlines_compared',0) for r in rows),flush=True)
