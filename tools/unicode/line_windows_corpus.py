"""Export Windows observations for native-only compatibility replay; retain dictionary exclusions."""
import argparse,csv,hashlib,json,struct,zipfile
from pathlib import Path
def identity(path):
    path=Path(path)
    return dict(path=str(path),bytes=path.stat().st_size,sha256=hashlib.sha256(path.read_bytes()).hexdigest())
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--root',required=True,type=Path);p.add_argument('--ucd',required=True,type=Path);a=p.parse_args()
    records=[];inputs=[];groups={};excluded=[]
    with zipfile.ZipFile(a.ucd) as z:raw=z.read('auxiliary/LineBreakTest.txt').decode('utf-8')
    official={i:[int(x,16) for x in line.split('#')[0].split()[1::2]] for i,line in enumerate(raw.splitlines(),1) if line.split('#')[0].strip()}
    def rows(path):
        inputs.append(identity(path));return list(csv.DictReader(path.open(encoding='utf-8',newline='')))
    data=rows(a.root/'windows-release.csv');assert len(data)==len(official)==19338
    for r in data:
        assert r['has_expected']=='1';cp=official.pop(int(r['source_line']));w=list(map(int,r['windows'].split()));candidate=list(map(int,r['candidate'].split()))
        assert w==candidate and len(w)==len(cp)+1;records.append((cp,w))
    assert not official;groups['official']=len(data)
    for folder in ('probes','holdout','validation','final-validation'):
        root=a.root/folder;manifest=json.loads((root/'probes.json').read_text());comparison=json.loads((root/'release-comparison.json').read_text())
        inputs.extend([identity(root/'probes.json'),identity(root/'release-comparison.json')]);assert identity(root/'probes.bin')==manifest['input']==comparison['input']
        source=manifest['rows'];data=rows(root/'release.csv');assert len(data)==len(source);seen=set();added=0
        for r in data:
            index=int(r['source_line'])-1;assert index not in seen and 0<=index<len(source) and r['has_expected']=='0';seen.add(index)
            row=source[index]
            if row['kind']=='dictionary':excluded.append(dict(folder=folder,id=index+1,differences=int(r['candidate_differences'])));continue
            w=list(map(int,r['windows'].split()));candidate=list(map(int,r['candidate'].split()));cp=row['scalars']
            assert w==candidate and len(w)==len(cp)+1 and all(v in (0,1,2) for v in w),(folder,index+1)
            records.append((cp,w));added+=1
        groups[folder]=added
    output=bytearray(b'MCWINLB1'+struct.pack('<I',len(records)))
    for cp,w in records:output+=struct.pack('<I',len(cp))+struct.pack('<'+'I'*len(cp),*cp)+bytes(w)
    path=a.root/'native-corpus.bin';path.write_bytes(output)
    report=dict(ucd=identity(a.ucd),inputs=inputs,output=identity(path),cases=len(records),boundary_checks=sum(len(w) for _,w in records),groups=groups,excluded_dictionary_cases=excluded)
    (a.root/'native-corpus.json').write_text(json.dumps(report,indent=2));print(json.dumps({k:v for k,v in report.items() if k not in ('inputs','excluded_dictionary_cases')}))
if __name__=='__main__':main()
