"""Capture original layout subtables and independent fontTools query expectations.

fontTools is QA-only. No table is recompiled: reader positions identify slices
of original SFNT table bytes, checked again against the font's table reader.
"""
import argparse
import collections
import hashlib
import json
from pathlib import Path
import struct
import fontTools
from fontTools.ttLib import TTFont
from fontTools.ttLib.tables import otTables


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('fonts',nargs='+',type=Path)
    parser.add_argument('--out',required=True,type=Path)
    args=parser.parse_args();args.out.parent.mkdir(parents=True,exist_ok=True)
    records=[];sources=[];fonts=[];seen=set();current={};formats=collections.Counter()
    def wrap(cls,kind):
        original=cls.decompile
        def read(self,reader,font):
            start=reader.pos
            original(self,reader,font)
            raw=bytes(reader.data[start:reader.pos]);tag=str(reader.tableTag)
            assert bytes(font.reader[tag][start:reader.pos])==raw
            key=(current['index'],tag,start,kind)
            if key in seen:return
            seen.add(key)
            if kind==1:
                expected=[(font.getGlyphID(name),i) for i,name in enumerate(self.glyphs)];aux=len(expected)
            elif kind==2:
                expected=sorted((font.getGlyphID(name),value) for name,value in self.classDefs.items());aux=max(self.classDefs.values(),default=0)
            else:
                if self.DeltaFormat==0x8000:
                    expected=[];aux=(self.StartSize<<16)|self.EndSize;kind_actual=4
                else:
                    expected=[(self.StartSize+i,value) for i,value in enumerate(self.DeltaValue)];aux=0;kind_actual=3
            actual=kind if kind<3 else kind_actual
            fmt=int.from_bytes(raw[:2] if kind<3 else raw[4:6],'big');formats[f'{actual}:{fmt}']+=1
            records.append((actual,current['glyphs'],raw,expected,aux,len(sources)))
            sources.append({'font_index':current['index'],'table':tag,'offset':start,'bytes':len(raw),'kind':actual,'format':fmt,'sha256':hashlib.sha256(raw).hexdigest()})
        cls.decompile=read
    wrap(otTables.Coverage,1);wrap(otTables.ClassDef,2);wrap(otTables.Device,3)
    for path in args.fonts:
        data=path.read_bytes();faces=struct.unpack_from('>I',data,8)[0] if data[:4]==b'ttcf' else 1
        for face in range(faces):
            with TTFont(path,fontNumber=face,lazy=False) as font:
                current={'index':len(fonts),'glyphs':len(font.getGlyphOrder())}
                assert current['glyphs']==font['maxp'].numGlyphs
                fonts.append({'path':str(path.resolve()),'face':face,'glyphs':current['glyphs'],'sha256':hashlib.sha256(data).hexdigest()})
                for tag in ['GSUB','GPOS','GDEF','BASE','MATH']:
                    if tag in font:
                        table=font[tag]
                        if hasattr(table,'table'):table.table.ensureDecompiled(recurse=True)
    with args.out.open('wb') as out:
        out.write(b'MCOTCM1\0'+struct.pack('<I',len(records)))
        for kind,limit,raw,expected,aux,ident in records:
            out.write(struct.pack('<6I',kind,limit,len(raw),len(expected),aux,ident));out.write(raw)
            for key,value in expected:out.write(struct.pack('<II',key,value&0xffffffff))
    result={'fontTools_version':fontTools.__version__,'fonts':fonts,'subtables':sources,'record_count':len(records),
            'formats':dict(formats),'corpus_sha256':hashlib.sha256(args.out.read_bytes()).hexdigest(),
            'scope':'original Coverage, ClassDef, Device and VariationIndex bytes; no shaping/positioning execution'}
    args.out.with_suffix('.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({'fonts':len(fonts),'records':len(records),'formats':dict(formats)}))


if __name__=='__main__':
    main()
