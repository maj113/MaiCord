"""Compare actual app ShapedText and measurement output through native font selection."""
from pathlib import Path
import argparse,hashlib,json,subprocess
p=argparse.ArgumentParser(description=__doc__);p.add_argument('root',type=Path);p.add_argument('--config',choices=['Release'],default='Release');p.add_argument('--fonts',type=Path,required=True);a=p.parse_args()
fonts=json.loads(a.fonts.read_text());rows=[]
for item in fonts:
 font=item['font'];path=Path(font['path']);assert hashlib.sha256(path.read_bytes()).hexdigest()==font['sha256']
 for weight in ([0,400,600] if path.stem.lower()=='bahnschrift' else [0]):
  result=subprocess.run([str(a.root/f'MaiCordNativeAppLineTests.exe'),str(path),str(weight)],capture_output=True,text=True)
  rows.append(dict(font=font,weight=weight,exit=result.returncode,stdout=result.stdout,stderr=result.stderr))
  if result.returncode:print(path.name,weight,result.stdout,flush=True)
failures=[r for r in rows if r['exit']];suffix=a.config.lower()
(a.root/f'app-comparison-{suffix}.json').write_text(json.dumps(rows,indent=2));(a.root/f'app-failures-{suffix}.json').write_text(json.dumps(failures,indent=2))
print(a.config,len(rows),'font instances;',len(failures),'failures')
if failures:raise SystemExit(1)
