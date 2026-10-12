"""Transcribe every official Unicode 17 line-break test; no algorithm here."""
import argparse,hashlib,json,zipfile
from pathlib import Path
PIN='2066d1909b2ea93916ce092da1c0ee4808ea3ef8407c94b4f14f5b7eb263d28e'
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--ucd',required=True,type=Path);p.add_argument('--output',required=True,type=Path);p.add_argument('--report',required=True,type=Path)
    a=p.parse_args()
    assert hashlib.sha256(a.ucd.read_bytes()).hexdigest()==PIN,'Wrong pinned Unicode archive'
    with zipfile.ZipFile(a.ucd) as z:data=z.read('auxiliary/LineBreakTest.txt')
    vectors=[];cases=scalars=maximum=0
    for line,raw in enumerate(data.decode('utf-8').splitlines(),1):
        text=raw.split('#')[0].strip()
        if not text:continue
        tokens=text.split()
        assert len(tokens)%2==1 and all(x in ('\u00f7','\u00d7') for x in tokens[::2]),line
        cps=[int(x,16) for x in tokens[1::2]]
        assert cps and all(cp<=0x10ffff and not 0xd800<=cp<=0xdfff for cp in cps),line
        vectors.extend([len(cps)|(0x80000000 if tokens[-1]=='\u00f7' else 0),line])
        vectors.extend(cp|(0x80000000 if tokens[2*i]=='\u00f7' else 0) for i,cp in enumerate(cps))
        cases+=1;scalars+=len(cps);maximum=max(maximum,len(cps))
    content='// Generated from pinned Unicode 17 LineBreakTest.txt; do not edit.\n'
    content+='// Unicode License v3; Copyright (c) 1991-2025 Unicode, Inc.\n// See assets/licenses/Unicode-3.0.txt.\n'
    content+=f'static constexpr u32 line_vector_count={cases};\nstatic constexpr u32 line_vectors[]={{\n'
    content+=''.join('    '+','.join(f'0x{x:08x}u' for x in vectors[i:i+10])+',\n' for i in range(0,len(vectors),10))+'};\n'
    a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(content,encoding='utf-8',newline='\n')
    result=dict(unicode_version='17.0.0',ucd=dict(path=str(a.ucd),sha256=PIN),source_sha256=hashlib.sha256(data).hexdigest(),cases=cases,scalars=scalars,boundary_checks=scalars+cases,maximum_case_scalars=maximum,generated_sha256=hashlib.sha256(a.output.read_bytes()).hexdigest())
    a.report.parent.mkdir(parents=True,exist_ok=True);a.report.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');print(json.dumps(result))
if __name__=='__main__':main()
