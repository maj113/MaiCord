"""Validate complete Windows line-break observations against the official corpus."""
import argparse,collections,csv,hashlib,json,os,re,zipfile
from pathlib import Path
PIN='2066d1909b2ea93916ce092da1c0ee4808ea3ef8407c94b4f14f5b7eb263d28e'
def identity(path):
    path=Path(path)
    return dict(path=str(path),bytes=path.stat().st_size,sha256=hashlib.sha256(path.read_bytes()).hexdigest())
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--ucd',required=True,type=Path);p.add_argument('--csv',required=True,type=Path)
    p.add_argument('--executable',required=True,type=Path);p.add_argument('--output',required=True,type=Path)
    a=p.parse_args();assert identity(a.ucd)['sha256']==PIN
    with zipfile.ZipFile(a.ucd) as z:
        data=z.read('auxiliary/LineBreakTest.txt').decode('utf-8');scripts=z.read('Scripts.txt').decode('utf-8')
    script=['Unknown']*0x110000
    for raw in scripts.splitlines():
        body=raw.split('#')[0].strip()
        if not body:continue
        span,name=map(str.strip,body.split(';'));bounds=span.split('..');first=int(bounds[0],16);last=int(bounds[-1],16)
        script[first:last+1]=[name]*(last-first+1)
    corpus={}
    for line,raw in enumerate(data.splitlines(),1):
        body,_,comment=raw.partition('#');tokens=body.split()
        if not tokens:continue
        cps=[int(x,16) for x in tokens[1::2]];expected=[int(x=='\u00f7') for x in tokens[::2]]
        rules=re.findall(r'\[([\d.]+)\]',comment);assert len(rules)==len(expected)
        corpus[line]=dict(cps=cps,expected=expected,rules=rules,comment=comment.strip())
    seen=set();counts=collections.Counter();rules=collections.Counter();failures=[];endpoints=collections.Counter();candidate_counts=collections.Counter();candidate_failures=[]
    for r in csv.DictReader(a.csv.open(encoding='utf-8',newline='')):
        assert 'has_expected' not in r or r['has_expected']=='1','Probe observations are not official conformance cases'
        line=int(r['source_line']);assert line in corpus and line not in seen;seen.add(line);case=corpus[line];cps=case['cps'];n=len(cps)
        native=[int(v) for v in r['native'].split()];windows=[int(v) for v in r['windows'].split()]
        before=[int(v) for v in r['before'].split()];after=[int(v) for v in r['after'].split()]
        white=[int(v) for v in r['whitespace'].split()];hyphen=[int(v) for v in r['soft_hyphen'].split()]
        starts=[0]
        for cp in cps:starts.append(starts[-1]+(1 if cp<=0xffff else 2))
        units=starts[-1];assert int(r['scalars'])==n and int(r['utf16_units'])==units
        assert len(native)==len(windows)==n+1 and all(len(v)==units for v in (before,after,white,hyphen))
        assert all(v in (0,1,2) for v in native) and [int(v!=0) for v in native]==case['expected']
        assert native[0]==0 and native[-1]==2
        assert all(v in (0,1,2,3) for v in before+after) and all(v in (0,1) for v in white+hyphen)
        def boundary(at):return (255,1,0,2)[max(after[at-1] if at else 0,before[at] if at<units else 0)]
        assert windows==[boundary(s) for s in starts]
        diff=[i for i in range(1,n) if windows[i]!=255 and windows[i]!=native[i]]
        unresolved=sum(windows[i]==255 for i in range(1,n));surrogates=sum(boundary(starts[i]+1) in (1,2) for i,cp in enumerate(cps) if cp>0xffff)
        assert int(r['checked'])==n-1-unresolved and int(r['differences'])==len(diff) and int(r['unresolved'])==unresolved and int(r['surrogate_breaks'])==surrogates
        counts.update(cases=1,scalars=n,interior_boundaries=n-1,checked=n-1-unresolved,differences=len(diff),unresolved=unresolved,surrogate_breaks=surrogates,exact_cases=int(not diff and not unresolved))
        endpoints[f'{windows[0]}/{windows[-1]}']+=1
        latin=any(script[cp]=='Latin' for cp in cps) and all(script[cp] in ('Latin','Common','Inherited') for cp in cps)
        if latin:counts.update(latin_common_inherited_cases=1,latin_common_inherited_exact=int(not diff and not unresolved),latin_common_inherited_differences=len(diff))
        if diff:
            rules.update(case['rules'][i] for i in diff)
            failures.append(dict(source_line=line,scalars=cps,native=native,windows=windows,positions=diff,rules=[case['rules'][i] for i in diff],latin_common_inherited=latin,comment=case['comment']))
        if 'candidate' in r:
            candidate=[int(v) for v in r['candidate'].split()];assert len(candidate)==n+1 and all(v in (0,1,2) for v in candidate)
            cd=[i for i in range(1,n) if windows[i]!=255 and candidate[i]!=windows[i]]
            fixed=[i for i in diff if i not in cd];regressed=[i for i in cd if i not in diff]
            assert int(r['candidate_differences'])==len(cd)
            candidate_counts.update(cases=1,differences=len(cd),exact_cases=int(not cd and not unresolved),fixed=len(fixed),regressed=len(regressed),endpoint_differences=sum(candidate[i]!=windows[i] for i in (0,n)))
            if latin:candidate_counts.update(latin_cases=1,latin_exact=int(not cd and not unresolved),latin_differences=len(cd),latin_regressed=len(regressed))
            if cd:candidate_failures.append(dict(source_line=line,scalars=cps,native=native,windows=windows,candidate=candidate,positions=cd,regressed=regressed,latin_common_inherited=latin,comment=case['comment']))
    assert seen==set(corpus) and len(seen)==19338
    report=dict(unicode_version='17.0.0',ucd=identity(a.ucd),csv=identity(a.csv),executable=identity(a.executable),dwrite=identity(Path(os.environ['WINDIR'])/'System32/dwrite.dll'),counts=dict(counts),differences_by_expected_rule=dict(rules.most_common()),endpoint_pairs=dict(endpoints),failures=failures,candidate_counts=dict(candidate_counts),candidate_failures=candidate_failures)
    a.output.write_text(json.dumps(report,indent=2),encoding='utf-8')
    print(json.dumps(dict(counts=report['counts'],differences_by_expected_rule=report['differences_by_expected_rule'],endpoint_pairs=report['endpoint_pairs'],candidate_counts=report['candidate_counts'])))
if __name__=='__main__':main()
