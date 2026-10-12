"""Transcribe the complete official UAX #9 test suites; no bidi algorithm here."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zipfile

PIN = '2066d1909b2ea93916ce092da1c0ee4808ea3ef8407c94b4f14f5b7eb263d28e'


def main():
    p=argparse.ArgumentParser()
    p.add_argument('--ucd',required=True,type=Path)
    p.add_argument('--schema',required=True,type=Path)
    p.add_argument('--output',required=True,type=Path)
    p.add_argument('--report',required=True,type=Path)
    a=p.parse_args()
    if hashlib.sha256(a.ucd.read_bytes()).hexdigest()!=PIN:raise ValueError('Incorrect pinned UCD archive')
    schema=json.loads(a.schema.read_text())
    names=next(f['values'] for f in schema['fields'] if f['ucd']=='bc')
    if schema['unicode_version']!='17.0.0':raise ValueError('Wrong data schema')
    counts=[0,0];scalar_count=0;maximum=0;sources={}
    with a.output.open('wb') as out,zipfile.ZipFile(a.ucd) as z:
        out.write(b'MCBIDI1\0'+bytes(4))
        def emit(cps,types,levels,visual,direction,base,line,kind):
            nonlocal scalar_count,maximum
            n=len(cps)
            if len(types)!=n or len(levels)!=n or len(visual)>n:raise ValueError(f'Bad vector at {line}')
            if sorted(visual)!=[i for i,level in enumerate(levels) if level!=255]:raise ValueError(f'Invalid visual index set at {line}')
            out.write(struct.pack('<6I',n,direction,base,len(visual),line,kind))
            for cp,ty,level in zip(cps,types,levels):out.write(struct.pack('<IBB2x',cp,ty,level))
            out.write(struct.pack('<'+'I'*len(visual),*visual))
            counts[kind]+=1;scalar_count+=n;maximum=max(maximum,n)
        for filename in ['BidiTest.txt','BidiCharacterTest.txt']:
            data=z.read(filename);sources[filename]=hashlib.sha256(data).hexdigest()
            levels=visual=None
            for line,raw in enumerate(data.decode('utf-8').splitlines(),1):
                value=raw.partition('#')[0].strip()
                if not value:continue
                if filename=='BidiTest.txt':
                    if value.startswith('@Levels:'):
                        levels=[255 if v=='x' else int(v) for v in value.split(':',1)[1].split()];continue
                    if value.startswith('@Reorder:'):
                        visual=[int(v) for v in value.split(':',1)[1].split()];continue
                    if value.startswith('@'):raise ValueError(f'Unknown directive {value}')
                    fields=[f.strip() for f in value.split(';')]
                    types=[names.index(v) for v in fields[0].split()];mask=int(fields[1],16)
                    if levels is None or visual is None or not 1<=mask<=7:raise ValueError('Missing expectations')
                    for bit,direction in [(1,2),(2,0),(4,1)]:
                        if mask&bit:emit([0]*len(types),types,levels,visual,direction,255,line,0)
                else:
                    fields=[f.strip() for f in value.split(';')]
                    cps=[int(v,16) for v in fields[0].split()]
                    if any(v>0x10ffff or 0xd800<=v<=0xdfff for v in cps):raise ValueError('Invalid test scalar')
                    levels=[255 if v=='x' else int(v) for v in fields[3].split()]
                    emit(cps,[255]*len(cps),levels,[int(v) for v in fields[4].split()],int(fields[1]),int(fields[2]),line,1)
        out.seek(8);out.write(struct.pack('<I',sum(counts)))
    result={'unicode_version':'17.0.0','class_cases':counts[0],'character_cases':counts[1],
            'total_cases':sum(counts),'scalar_inputs':scalar_count,'maximum_case_scalars':maximum,
            'source_sha256':sources,'corpus_sha256':hashlib.sha256(a.output.read_bytes()).hexdigest()}
    a.report.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result))


if __name__=='__main__':main()
