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
        attach=g.AttachList if g else None;carets=g.LigCaretList if g else None;sets=getattr(g,'MarkGlyphSetsDef',None) if g else None;store=getattr(g,'VarStore',None) if g else None
        ac=len(attach.Coverage.glyphs) if attach else 0;cc=len(carets.Coverage.glyphs) if carets else 0;sc=len(sets.Coverage) if sets else 0
        flags=bool(g)|(bool(attach)<<1)|(bool(carets)<<2)|(bool(sets)<<3)|(bool(store)<<4)
        blob=bytearray(struct.pack('<6I',n,len(raw),flags,ac,cc,sc)+raw)
        for c,m in zip(classes,marks):blob+=struct.pack('<HH',c,m)
        if attach:
            for name,points in zip(attach.Coverage.glyphs,attach.AttachPoint):
                blob+=struct.pack('<II',font.getGlyphID(name),len(points.PointIndex));blob+=struct.pack('<'+'I'*len(points.PointIndex),*points.PointIndex)
        if carets:
            for name,lig in zip(carets.Coverage.glyphs,carets.LigGlyph):
                blob+=struct.pack('<II',font.getGlyphID(name),len(lig.CaretValue))
                for cv in lig.CaretValue:
                    dev=getattr(cv,'DeviceTable',None);value=cv.CaretValuePoint if cv.Format==2 else cv.Coordinate
                    fmt=dev.DeltaFormat if dev else 0;start=dev.StartSize if dev else 0;end=dev.EndSize if dev else 0;deltas=dev.DeltaValue if dev and fmt!=0x8000 else []
                    blob+=struct.pack('<6I',cv.Format,value&0xffffffff,fmt,start,end,len(deltas))
                    for d in deltas:blob+=struct.pack('<i',d)
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
        metadata.append({**identity,'gdef_sha256':hashlib.sha256(raw).hexdigest(),'version':g.Version if g else None,'glyphs':n,'attachments':ac,'caret_glyphs':cc,'mark_sets':sc,'variation_store':bool(store),'filters':len(configs)})
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
    a.out.write_bytes(b'MCGDEF1\0'+struct.pack('<I',len(records))+b''.join(records))
    meta={'fontTools_version':fontTools.__version__,'records':metadata,'corpus_sha256':hashlib.sha256(a.out.read_bytes()).hexdigest()};a.out.with_suffix('.json').write_text(json.dumps(meta,indent=2)+'\n');print(f'Captured {len(records)} GDEF records')

if __name__=='__main__':main()
