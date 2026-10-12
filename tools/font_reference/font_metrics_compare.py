"""Compare owned native layout metrics against the captured app font matrix."""
from pathlib import Path
import argparse,hashlib,json,subprocess

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('root',type=Path);p.add_argument('--config',choices=['Release','Debug'],required=True);a=p.parse_args()
    rows=[];differences=[]
    for case in json.loads((a.root/'app-metrics.json').read_text()):
        font=case['font'];path=Path(font['path']);assert hashlib.sha256(path.read_bytes()).hexdigest()==font['sha256']
        result=subprocess.run([str(a.root/f'MaiCordRunPlanTests.exe'),str(path),str(case['weight']),'metrics'],capture_output=True,text=True,check=True)
        actual=json.loads(result.stdout.splitlines()[0]);m=case['metrics']['font_metrics']
        expected=dict(metrics=m[:4]+m[6:],lines=[line['observed']['layout_lines'][0][3:] for line in case['layouts']])
        row=dict(font=font,weight=case['weight'],expected=expected,native=actual,output=result.stdout);rows.append(row)
        if actual!=expected:differences.append(row)
    suffix=a.config.lower();(a.root/f'app-{suffix}.json').write_text(json.dumps(rows,indent=2));(a.root/f'differences-{suffix}.json').write_text(json.dumps(differences,indent=2))
    print(f'{a.config}: {len(rows)} font instances; {len(rows)*3} line-size cases; {len(differences)} differences')
    if differences:raise SystemExit(1)

if __name__=='__main__':main()
