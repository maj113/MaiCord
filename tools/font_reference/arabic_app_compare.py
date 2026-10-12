"""Compare native Arabic diagnostics with recorded Windows glyphs/maps/placements."""
import argparse,collections,json
from pathlib import Path
from gsub_execution_oracle import identity

def compare(reference,native,output,single_gate=False):
    data=json.loads(reference.read_text());cases=data['cases'];rows=[json.loads(line) for line in native.read_text().splitlines() if line.startswith('{')]
    assert len(rows)==len(cases)*(1 if single_gate else 2);indexed={(r['case'],r['gate']):r for r in rows};assert len(indexed)==len(rows)
    counts=collections.Counter();remaining=collections.Counter();details=[]
    for i,c in enumerate(cases):
        a=indexed[i,1 if single_gate else 0]
        if not single_gate:
            b=indexed[i,1];assert {k:v for k,v in a.items() if k!='gate'}=={k:v for k,v in b.items() if k!='gate'}
        assert a['status']==0,(i,a['status']);different=[k for k in ['glyphs','clusters','placement_bits'] if a[k]!=c['result'][k]]
        counts['cases']+=1
        if not different:counts['exact']+=1
        else:remaining[str(c['sample'])]+=1
        details.append(dict(case=i,sample=c['sample'],name=c['name'],different=different))
    result=dict(reference=identity(reference),native=identity(native),cases=counts['cases'],exact_cases=counts['exact'],
        unmatched_cases=counts['cases']-counts['exact'],bounds_matrices_agree=0 if single_gate else len(cases),native_rows=len(rows),remaining_samples=dict(remaining),details=details)
    output.write_text(json.dumps(result,indent=2));print({k:v for k,v in result.items() if k not in ['details','reference','native']})

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('reference',type=Path);p.add_argument('native',type=Path);p.add_argument('--out',type=Path,required=True);p.add_argument('--single-gate',action='store_true');a=p.parse_args();compare(a.reference,a.native,a.out,a.single_gate)
