"""Independent synthetic common-layout fixtures for formats absent in real fonts."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import fontTools
from fontTools.ttLib import TTFont
from fontTools.ttLib.tables import otTables
from fontTools.ttLib.tables.otBase import OTTableReader,OTTableWriter


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out',required=True,type=Path)
    args=parser.parse_args();args.out.parent.mkdir(parents=True,exist_ok=True)
    font=TTFont();font.setGlyphOrder([f'g{i}' for i in range(65536)])
    records=[];descriptions=[]
    def words(*values):return struct.pack('>'+'H'*len(values),*values)
    def add(kind,raw,name,limit=65536):
        cls={1:otTables.Coverage,2:otTables.ClassDef,3:otTables.Device}[kind]
        obj=cls();reader=OTTableReader(raw,tableTag='TEST');obj.decompile(reader,font);assert reader.pos==len(raw)
        if kind==1:expected=[(font.getGlyphID(g),i) for i,g in enumerate(obj.glyphs)];aux=len(expected)
        elif kind==2:expected=sorted((font.getGlyphID(g),v) for g,v in obj.classDefs.items());aux=max(obj.classDefs.values(),default=0)
        elif obj.DeltaFormat==0x8000:kind=4;expected=[];aux=(obj.StartSize<<16)|obj.EndSize
        else:expected=[(obj.StartSize+i,v) for i,v in enumerate(obj.DeltaValue)];aux=0
        ident=len(records);records.append((kind,limit,raw,expected,aux,ident))
        descriptions.append({'name':name,'kind':kind,'glyph_limit':limit,'bytes':len(raw),'expected_entries':len(expected),'sha256':hashlib.sha256(raw).hexdigest()})
    add(1,words(1,0),'coverage1-empty')
    add(1,words(1,3,0,1,65535),'coverage1-extremes')
    add(1,words(2,2,0,2,0,65534,65535,3),'coverage2-extremes')
    add(1,words(2,1,0,65535,0),'coverage2-full-16-bit-domain')
    add(2,words(1,65533,3,65535,0,32768),'class1-extremes')
    add(2,words(1,65535,0),'class1-empty-unused-start',16)
    add(2,words(2,3,0,0,65535,5,7,0,65530,65535,42),'class2-explicit-zero-and-extremes')
    add(2,words(2,0),'class2-empty')
    for fmt in (1,2,3):
        bits=1<<fmt;values=list(range(-(1<<(bits-1)),1<<(bits-1)))+[1,0,-1]
        for start in (0,50,65536-len(values)):
            obj=otTables.Device();obj.StartSize=start;obj.EndSize=start+len(values)-1;obj.DeltaFormat=fmt;obj.DeltaValue=values
            writer=OTTableWriter(tableTag='TEST');obj.compile(writer,font)
            add(3,writer.getAllData(),f'device{fmt}-start-{start}')
    for outer,inner in ((0,0),(65535,1),(1,65535),(65535,65535)):
        add(3,words(outer,inner,0x8000),f'variation-{outer}-{inner}')
    with args.out.open('wb') as out:
        out.write(b'MCOTCM1\0'+struct.pack('<I',len(records)))
        for kind,limit,raw,expected,aux,ident in records:
            out.write(struct.pack('<6I',kind,limit,len(raw),len(expected),aux,ident));out.write(raw)
            for key,value in expected:out.write(struct.pack('<II',key,value&0xffffffff))
    result={'kind':'synthetic; not original font tables','fontTools_version':fontTools.__version__,'records':descriptions,'record_count':len(records),'corpus_sha256':hashlib.sha256(args.out.read_bytes()).hexdigest()}
    args.out.with_suffix('.json').write_text(json.dumps(result,indent=2)+'\n');print(f'Generated {len(records)} independently decoded fixtures')


if __name__=='__main__':
    main()
