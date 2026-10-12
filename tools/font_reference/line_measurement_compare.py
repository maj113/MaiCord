"""Replay actual-font line/caret expectations through the native frontend."""
from pathlib import Path
import argparse,hashlib,json,struct,subprocess
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('root',type=Path);p.add_argument('--config',choices=['Release','Debug'],required=True);a=p.parse_args()
    rows=[];differences=[];verified=set()
    for case in json.loads((a.root/'observed.json').read_text()):
        font=case['font'];path=Path(font['path'])
        if path not in verified:assert hashlib.sha256(path.read_bytes()).hexdigest()==font['sha256'];verified.add(path)
        bits=struct.unpack('<I',struct.pack('<f',case['size']))[0];cps=','.join(f'{ord(c):x}' for c in case['text'])
        result=subprocess.run([str(a.root/f'MaiCordRunPlanTests.exe'),str(path),str(case['weight']),f'line:{bits}:{int(case["independent"])}:{cps}'],capture_output=True,text=True,check=True)
        actual=json.loads(result.stdout.splitlines()[0]);o=case['observed'];clusters=[];at=0
        for c in o['cluster_metrics']:
            hit=o['position_hits'][at*2];at+=c[0];clusters.append([at,hit[6],c[1]])
        expected=dict(glyphs=o['layout_glyphs'],clusters=o['layout_clusters'],placement_bits=o['layout_placement_bits'],
            line_metrics=o['layout_metrics_bits']+[o['layout_lines'][0][4],o['layout_lines'][0][1]],cluster_extents=clusters,
            position_hits=[[h[2],h[3],h[4],h[6],h[8]] for h in o['position_hits']])
        row={k:case[k] for k in ['font','weight','text','independent','size']};row.update(expected=expected,native=actual);rows.append(row)
        if actual['status']!=0 or any(actual[k]!=v for k,v in expected.items()):differences.append(row)
    suffix=a.config.lower();(a.root/f'comparison-{suffix}.json').write_text(json.dumps(rows,indent=2));(a.root/f'differences-{suffix}.json').write_text(json.dumps(differences,indent=2))
    hits=sum(len(row['expected']['position_hits']) for row in rows)
    print(f'{a.config}: {len(rows)} line cases; {hits} source-position hits; {len(differences)} differing cases')
    if differences:raise SystemExit(1)
if __name__=='__main__':main()
