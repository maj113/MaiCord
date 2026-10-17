"""Capture original GDEF bytes and fontTools expectations; add labeled fixtures."""
import argparse,hashlib,json,struct
from pathlib import Path
import fontTools
from fontTools.ttLib import TTFont
from fontTools.ttLib.tables import otTables
from fontTools.ttLib.tables.otBase import OTTableReader

def synthetic(minor):
    w=lambda *v:struct.pack('>'+'H'*len(v),*v)
    data=bytearray(w(1,minor)+bytes((18 if minor==3 else 14 if minor==2 else 12)-4))
    def append(field,value,wide=False):
        struct.pack_into('>I' if wide else '>H',data,field,len(data));data.extend(value)
    append(4,w(1,0,5,0,1,2,3,4));append(10,w(1,3,2,2,1))
    append(6,w(6,1,12)+w(1,1,2)+w(3,1,5,9))
    values=[w(1,65506),w(2,5),w(3,100,6)+w(16,16,1,0xc000),w(3,300,0)]
    if minor==3:values.append(w(3,200,6)+w(0,0,0x8000))
    lig=bytearray(w(len(values))+bytes(2*len(values)))
    for i,value in enumerate(values):struct.pack_into('>H',lig,2+2*i,len(lig));lig.extend(value)
    append(8,w(6,1,12)+w(1,1,2)+lig)
    if minor>=2:append(12,w(1,2)+struct.pack('>II',12,20)+w(1,2,3,4)+w(2,1,3,3,0))
    if minor==3:append(14,struct.pack('>HIHI',1,12,1,22)+struct.pack('>HHhhh',1,1,0,16384,16384)+struct.pack('>HHHHh',1,1,1,0,40),True)
    return bytes(data)

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--font-manifest',required=True,type=Path);p.add_argument('--out',required=True,type=Path);a=p.parse_args();a.out.parent.mkdir(parents=True,exist_ok=True)
    sources=json.loads(a.font_manifest.read_text())['fonts'];records=[];metadata=[]
    def emit(font,raw,g,identity):
        n=len(font.getGlyphOrder());classes=[0]*n;marks=[0]*n
        if g and g.GlyphClassDef:
            for name,value in g.GlyphClassDef.classDefs.items():classes[font.getGlyphID(name)]=value
        if g and g.MarkAttachClassDef:
            for name,value in g.MarkAttachClassDef.classDefs.items():marks[font.getGlyphID(name)]=value
        sets=getattr(g,'MarkGlyphSetsDef',None) if g else None;store=getattr(g,'VarStore',None) if g else None
        sc=len(sets.Coverage) if sets else 0
        flags=bool(g)|(bool(sets)<<1)|(bool(store)<<2)
        blob=bytearray(struct.pack('<4I',n,len(raw),flags,sc)+raw)
        for c,m in zip(classes,marks):blob+=struct.pack('<HH',c,m)
        glyphsets=[set(font.getGlyphID(name) for name in cov.glyphs) for cov in sets.Coverage] if sets else []
        for glyphs in glyphsets:blob+=struct.pack('<I',len(glyphs))+struct.pack('<'+'I'*len(glyphs),*sorted(glyphs))
        blob+=struct.pack('<3I',store.VarRegionList.RegionAxisCount if store else 0,len(store.VarRegionList.Region) if store else 0,len(store.VarData) if store else 0)
        configs=[]
        for low in range(32):
            for mt in (0,1,2,255):
                for selected in (range(sc) if low&16 else [0]):
                    bits=bytearray((n+7)//8)
                    for glyph,(cl,ma) in enumerate(zip(classes,marks)):
                        ignore=(cl==1 and low&2) or (cl==2 and low&4)
                        if cl==3:ignore=bool(low&8) or (glyph not in glyphsets[selected] if low&16 else bool(mt and ma!=mt))
                        if ignore:bits[glyph//8]|=1<<(glyph%8)
                    configs.append(struct.pack('<II',low|(mt<<8),selected)+bits)
        blob+=struct.pack('<I',len(configs))+b''.join(configs);records.append(blob)
        metadata.append({**identity,'gdef_sha256':hashlib.sha256(raw).hexdigest(),'version':g.Version if g else None,'glyphs':n,'mark_sets':sc,'variation_store':bool(store),'filters':len(configs)})
    for source in sources:
        if hashlib.sha256(Path(source['path']).read_bytes()).hexdigest()!=source['sha256']:
            raise ValueError(f"Source font changed: {source['path']}")
        with TTFont(source['path'],fontNumber=source['face']) as font:
            if len(font.getGlyphOrder())!=source['glyphs'] or font['maxp'].numGlyphs!=source['glyphs']:
                raise ValueError(f"Glyph count changed: {source['path']}")
            raw=bytes(font.reader['GDEF']) if 'GDEF' in font else b'';g=font['GDEF'].table if raw else None
            emit(font,raw,g,source)
    for minor in (0,2,3):
        font=TTFont();font.setGlyphOrder(['g0','g1','g2','g3','g4']);raw=synthetic(minor);g=otTables.GDEF();g.decompile(OTTableReader(raw,tableTag='GDEF'),font);g.ensureDecompiled(recurse=True)
        emit(font,raw,g,{'synthetic':True,'name':f'GDEF-1.{minor}'})
    a.out.write_bytes(b'MCGDEF2\0'+struct.pack('<I',len(records))+b''.join(records))
    meta={'fontTools_version':fontTools.__version__,'records':metadata,'corpus_sha256':hashlib.sha256(a.out.read_bytes()).hexdigest()};a.out.with_suffix('.json').write_text(json.dumps(meta,indent=2)+'\n');print(f'Captured {len(records)} GDEF records')

if __name__=='__main__':main()
