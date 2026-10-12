"""Preserve pre-17 line properties for newly assigned Unicode 17 characters."""
import argparse,collections,hashlib,json,re,struct,zipfile
from pathlib import Path
PIN17='2066d1909b2ea93916ce092da1c0ee4808ea3ef8407c94b4f14f5b7eb263d28e'
PIN16='4be1c18e7b121d951018065b453bb05083f624c8f905ce8be7c0f354c9097c95'
def identity(path):return dict(path=str(path),bytes=path.stat().st_size,sha256=hashlib.sha256(path.read_bytes()).hexdigest())
def span(value):
    p=value.strip().split('..');return int(p[0],16),int(p[-1],16)
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--ucd',required=True,type=Path);p.add_argument('--previous',required=True,type=Path)
    p.add_argument('--output',required=True,type=Path);p.add_argument('--report',required=True,type=Path);p.add_argument('--probes',required=True,type=Path);a=p.parse_args()
    assert identity(a.ucd)['sha256']==PIN17 and identity(a.previous)['sha256']==PIN16
    with zipfile.ZipFile(a.ucd) as z:aliases=z.read('PropertyValueAliases.txt').decode();current=z.read('extracted/DerivedLineBreak.txt').decode();ages=z.read('DerivedAge.txt').decode()
    names={}
    for raw in aliases.splitlines():
        fields=[v.strip() for v in raw.split('#')[0].split(';')]
        if fields[0]=='lb':
            for value in fields[1:]:names[value]=fields[1]
    def properties(content):
        data=['XX']*0x110000
        for raw in content.splitlines():
            if '@missing:' not in raw:continue
            area,value=map(str.strip,raw.split('@missing:')[1].split(';'));first,last=span(area);data[first:last+1]=[names[value]]*(last-first+1)
        for raw in content.splitlines():
            body=raw.split('#')[0].strip()
            if not body:continue
            area,value=map(str.strip,body.split(';'));first,last=span(area);data[first:last+1]=[names[value]]*(last-first+1)
        return data
    old,new=properties(a.previous.read_text(encoding='utf-8')),properties(current);assigned=[]
    for raw in ages.splitlines():
        body=raw.split('#')[0].strip()
        if not body:continue
        area,age=map(str.strip,body.split(';'))
        if age=='17.0':first,last=span(area);assigned.extend(range(first,last+1))
    def effective(value):
        if value in ('AI','SG','XX','SA','AK','AS','AP'):return 'AL'
        return {'CJ':'NS','HH':'BA','VF':'CM','VI':'CM','EM':'CM','ZWJ':'GL'}.get(value,value)
    records=[];changed=0;raw_changed=0
    for cp in assigned:
        raw_changed+=old[cp]!=new[cp]
        if effective(old[cp])==effective(new[cp]):continue
        changed+=1
        if records and records[-1][0]+records[-1][1]==cp and records[-1][2]==old[cp]:records[-1][1]+=1
        else:records.append([cp,1,old[cp]])
    assert all(n<=65535 for _,n,_ in records)
    output='// Generated from pinned Unicode 16/17 data; do not edit. Unicode License v3.\n// Copyright (c) 1991-2025 Unicode, Inc. See assets/licenses/Unicode-3.0.txt.\n'
    output+='static constexpr LineClassOverride line_class_overrides[]={\n'+''.join(f'    {{0x{cp:x}u,{n},B::{kind.lower()}}},\n' for cp,n,kind in records)+'};\n'
    a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(output,encoding='utf-8',newline='\n')
    rows=[]
    for cp in assigned:
        for left,right in ((0x61,0x62),(0x4e2d,0x4e2d),(0x24,0x25),(0x20,0x61),(0x61,0x301),(0x201c,0x201d)):rows.append(dict(kind='age17',scalars=[left,cp,right]))
    data=bytearray(b'MCLBPRB1'+struct.pack('<I',len(rows)))
    for i,row in enumerate(rows,1):data+=struct.pack('<IIIII',i,3,*row['scalars'])
    a.probes.mkdir(parents=True,exist_ok=True);(a.probes/'probes.bin').write_bytes(data)
    manifest=dict(input=identity(a.probes/'probes.bin'),cases=len(rows),groups=dict(age17=len(rows)),rows=rows)
    (a.probes/'probes.json').write_text(json.dumps(manifest,indent=2))
    report=dict(ucd=identity(a.ucd),previous=identity(a.previous),assigned=len(assigned),raw_changed=raw_changed,changed=changed,ranges=records,bytes=len(records)*8,generated=identity(a.output.resolve()),probe_cases=len(rows))
    a.report.write_text(json.dumps(report,indent=2));print(json.dumps({k:v for k,v in report.items() if k!='ranges'}))
if __name__=='__main__':main()
