"""Run native point/range checks on recorded actual-font query files."""
from pathlib import Path
import argparse,hashlib,json,re,subprocess
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('root',type=Path);p.add_argument('--config',choices=['Release','Debug'],required=True);a=p.parse_args()
    rows=[];failures=[];verified=set();points=ranges=0
    for index,case in enumerate(json.loads((a.root/'observed.json').read_text())):
        font=case['font'];path=Path(font['path'])
        if path not in verified:assert hashlib.sha256(path.read_bytes()).hexdigest()==font['sha256'];verified.add(path)
        fixture=a.root/f'cases/{index:04}.bin'
        result=subprocess.run([str(a.root/f'MaiCordRunPlanTests.exe'),str(path),str(case['weight']),f'q:{fixture}'],capture_output=True,text=True)
        row=dict(index=index,font=font,weight=case['weight'],text=case['text'],independent=case['independent'],size=case['size'],exit=result.returncode,output=result.stdout,stderr=result.stderr,fixture_sha256=hashlib.sha256(fixture.read_bytes()).hexdigest());rows.append(row)
        if result.returncode:failures.append(row)
        else:
            n=int(re.search('point_queries=([0-9]+)',result.stdout)[1]);m=int(re.search('range_queries=([0-9]+)',result.stdout)[1]);assert n==len(case['observed']['point_hits']) and m==len(case['observed']['range_hits']);points+=n;ranges+=m
    suffix=a.config.lower();(a.root/f'comparison-{suffix}.json').write_text(json.dumps(rows,indent=2));(a.root/f'failures-{suffix}.json').write_text(json.dumps(failures,indent=2))
    print(f'{a.config}: {len(rows)} cases; {points} point queries; {ranges} range queries; {len(failures)} failures')
    if failures:raise SystemExit(1)
if __name__=='__main__':main()
