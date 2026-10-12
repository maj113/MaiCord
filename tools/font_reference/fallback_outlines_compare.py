"""Audit normalized native outlines in every file observed by app_text_boundary.

This intentionally checks every face in each observed file. It does not test
synthetic styles, text shaping, color paint evaluation or visible raster output.
"""
from pathlib import Path
import argparse,hashlib,json,re,subprocess
p=argparse.ArgumentParser(description=__doc__);p.add_argument('root',type=Path);p.add_argument('--summary',default='summary-release-2.json');a=p.parse_args()
summary=json.loads((a.root/a.summary).read_text(encoding='utf-8'));folder=a.root/'fallback-outlines';folder.mkdir(exist_ok=True)
exe=a.root/'bin/Release/MaiCordFontReference.exe';rows=[]
for index,font in enumerate(summary['files']):
 path=Path(font['path']);assert hashlib.sha256(path.read_bytes()).hexdigest()==font['sha256']
 result=subprocess.run([str(exe),str(path),'--instructions'],capture_output=True,text=True,timeout=60)
 (folder/f'{index:02d}-{path.stem}.txt').write_text(result.stdout+result.stderr)
 metrics={k:int(v) for k,v in re.findall(r'^(\w+)=(\d+)$',result.stdout,re.M)}
 rows.append(dict(font=font,exit=result.returncode,metrics=metrics));print(path.name,result.returncode,metrics.get('outlines_compared'),metrics.get('outline_differences'),flush=True)
(a.root/'fallback-outlines.json').write_text(json.dumps(dict(scope=__doc__,executable=dict(path=str(exe),sha256=hashlib.sha256(exe.read_bytes()).hexdigest()),files=rows),indent=2))
print('files',len(rows),'failed',sum(bool(r['exit']) for r in rows),'outlines',sum(r['metrics'].get('outlines_compared',0) for r in rows),flush=True)
