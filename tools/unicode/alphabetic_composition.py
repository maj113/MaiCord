"""Pinned Latin/Greek/Cyrillic ordered composition data; not Unicode NFC."""
from pathlib import Path
import argparse,hashlib,json,zipfile
from latin_composition import PIN,WINDOWS_PAIRS
GREEK_PAIRS=[(a,b,c) for a,values in [(0x1fb3,(0x1fb2,0x1fb4,0x1fb7)),(0x1fc3,(0x1fc2,0x1fc4,0x1fc7)),(0x1ff3,(0x1ff2,0x1ff4,0x1ff7))] for b,c in zip((0x300,0x301,0x342),values)]
def render(pairs):
    assert len(pairs)<=65535
    text='// Unicode 17.0.0 ordered Latin/Greek/Cyrillic composition candidates.\n// Exclusions and 21 additional pairs; not a normalization policy.\n// Unicode-derived data: Unicode License v3. Copyright (c) 1991-2025 Unicode, Inc.\n// See assets/licenses/Unicode-3.0.txt; tools/unicode/alphabetic_composition.py.\n'
    first=min(y for x,y,z in pairs);last=max(y for x,y,z in pairs)
    grouped=sorted(pairs,key=lambda p:(p[1],p[0]));offsets=[];cursor=0
    for mark in range(first,last+2):
        while cursor<len(grouped) and grouped[cursor][1]<mark:cursor+=1
        offsets.append(cursor)
    text+=f'constexpr u32 alphabetic_composition_first_mark=0x{first:x};\n'
    for name,values in [('offsets',offsets),('bases',[x for x,y,z in grouped]),('values',[z for x,y,z in grouped])]:
        text+=f'constexpr u16 alphabetic_composition_{name}[]{{\n'+'\n'.join('    '+','.join(f'0x{v:04x}' for v in values[i:i+12])+',' for i in range(0,len(values),12))+'\n};\n'
    return text,len(grouped)*4+len(offsets)*2

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--ucd',required=True,type=Path);p.add_argument('--repo',type=Path,default=Path(__file__).resolve().parents[2]);p.add_argument('--report',required=True,type=Path);a=p.parse_args()
    assert hashlib.sha256(a.ucd.read_bytes()).hexdigest()==PIN
    with zipfile.ZipFile(a.ucd) as z:
        scripts=z.read('Scripts.txt').decode();normalization=z.read('DerivedNormalizationProps.txt').decode();unicode=z.read('UnicodeData.txt').decode()
    selected=set();excluded=set()
    for source,target,properties in [(scripts,selected,('Latin','Greek','Cyrillic')),(normalization,excluded,('Full_Composition_Exclusion',))]:
        for raw in source.splitlines():
            p=raw.split('#')[0].strip().split(';')
            if len(p)<2 or p[1].strip() not in properties:continue
            span=p[0].strip().split('..');target.update(range(int(span[0],16),int(span[-1],16)+1))
    canonical=[];omitted=[]
    for raw in unicode.splitlines():
        p=raw.split(';');cp=int(p[0],16);d=p[5].split()
        if cp not in selected or cp in excluded or len(d)!=2 or d[0].startswith('<'):continue
        x,y=(int(v,16) for v in d)
        # The reference preserves these spacing-mark sequences. Their Unicode
        # canonical compositions are not part of its ordered font preprocessor.
        (omitted if x in (0xa8,0x1fbf,0x1ffe) else canonical).append((x,y,cp))
    pairs=sorted(canonical+WINDOWS_PAIRS+GREEK_PAIRS);assert len({(x,y) for x,y,z in pairs})==len(pairs)
    assert all(0<=v<=65535 for row in pairs for v in row)
    text,table_bytes=render(pairs)
    out=a.repo/'src/text/generated/alphabetic_composition_data.inc';out.write_text(text,encoding='utf-8',newline='\n')
    a.report.write_text(json.dumps(dict(ucd=dict(path=str(a.ucd),sha256=PIN),canonical=canonical,omitted=omitted,windows_pairs=WINDOWS_PAIRS+GREEK_PAIRS,pairs=pairs,bytes=table_bytes,generated_sha256=hashlib.sha256(out.read_bytes()).hexdigest()),indent=2))
    print(len(pairs),'pairs;',table_bytes,'bytes')
if __name__=='__main__':main()
