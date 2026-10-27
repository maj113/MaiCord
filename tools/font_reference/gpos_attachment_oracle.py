"""Offline original-font GPOS attachment data oracle; no placement prediction."""
import argparse,collections,hashlib,json,struct
from pathlib import Path
import fontTools
from fontTools.ttLib import TTFont,newTable
from fontTools.ttLib.tables import otTables as ot
from fontTools.ttLib.tables.otBase import BaseTable,OTTableReader
from gsub_execution_oracle import obj,coverage,lookup
from gpos_oracle import device_blob
from embed_fixture import write_embedded_array
word=lambda *v:struct.pack('<'+'I'*len(v),*v)

def synthetic():
    f=TTFont();f.setGlyphOrder(['g0','g1','g2','g3','g4'])
    a=[obj('Anchor',Format=1,XCoordinate=-23,YCoordinate=45),obj('Anchor',Format=2,XCoordinate=17,YCoordinate=-9,AnchorPoint=7),
       obj('Anchor',Format=3,XCoordinate=120,YCoordinate=240,
           XDeviceTable=obj('Device',StartSize=16,EndSize=18,DeltaFormat=3,DeltaValue=[-128,127,-5]),
           YDeviceTable=obj('Device',StartSize=0xffff,EndSize=0xffff,DeltaFormat=0x8000))]
    marks=obj('MarkArray',MarkCount=2,MarkRecord=[obj('MarkRecord',Class=0,MarkAnchor=a[1]),obj('MarkRecord',Class=2,MarkAnchor=a[2])])
    cur=obj('CursivePos',Format=1,Coverage=coverage(['g1','g2','g3','g4']),EntryExitCount=4,
        EntryExitRecord=[obj('EntryExitRecord',EntryAnchor=x,ExitAnchor=y) for x,y in [(a[0],a[1]),(a[2],None),(None,a[0]),(None,None)]])
    base=obj('MarkBasePos',Format=1,MarkCoverage=coverage(['g3','g4']),BaseCoverage=coverage(['g1','g2']),ClassCount=3,MarkArray=marks,
        BaseArray=obj('BaseArray',BaseCount=2,BaseRecord=[obj('BaseRecord',BaseAnchor=[a[0],None,a[2]]),obj('BaseRecord',BaseAnchor=[None,a[1],None])]))
    lig=obj('MarkLigPos',Format=1,MarkCoverage=coverage(['g3','g4']),LigatureCoverage=coverage(['g1','g2']),ClassCount=3,MarkArray=marks,
        LigatureArray=obj('LigatureArray',LigatureCount=2,LigatureAttach=[obj('LigatureAttach',ComponentCount=len(rows),ComponentRecord=[obj('ComponentRecord',LigatureAnchor=row) for row in rows])
            for rows in [[[a[0],None,a[1]],[None,a[2],None],[a[1],None,a[0]]],[[None,None,None]]]]))
    mark=obj('MarkMarkPos',Format=1,Mark1Coverage=coverage(['g3','g4']),Mark2Coverage=coverage(['g3','g4']),ClassCount=3,Mark1Array=marks,
        Mark2Array=obj('Mark2Array',Mark2Count=2,Mark2Record=[obj('Mark2Record',Mark2Anchor=[a[2],None,a[0]]),obj('Mark2Record',Mark2Anchor=[None,a[1],None])]))
    subs=[cur,base,lig,mark];lookups=[lookup(i+3,s) for i,s in enumerate(subs)]
    lookups += [lookup(9,obj('ExtensionPos',Format=1,ExtensionLookupType=i+3,ExtSubTable=s)) for i,s in enumerate(subs)]
    lookups.append(lookup(3,cur,cur,flags=1))
    f['GPOS']=newTable('GPOS');f['GPOS'].table=obj('GPOS',Version=0x10000,ScriptList=obj('ScriptList',ScriptCount=0,ScriptRecord=[]),
        FeatureList=obj('FeatureList',FeatureCount=0,FeatureRecord=[]),LookupList=obj('LookupList',LookupCount=len(lookups),Lookup=lookups))
    return f,f.getTableData('GPOS'),struct.pack('>14H',1,0,12,0,0,0,1,0,5,0,1,2,3,3)

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--font-manifest',type=Path,required=True);p.add_argument('--out',type=Path,required=True);p.add_argument('--fixture-header',type=Path);args=p.parse_args()
    args.out.parent.mkdir(parents=True,exist_ok=True);positions={};active=[None];original=BaseTable.decompile;totals=collections.Counter();fonts=[];blobs=[];fixture=None
    def observed(self,reader,font):
        tracked=str(reader.tableTag)=='GPOS'
        if tracked:assert bytes(reader.data)==active[0];positions[id(self)]=reader.pos
        original(self,reader,font)
    BaseTable.decompile=observed
    def emit(font,raw,gd,identity):
        nonlocal fixture
        positions.clear();active[0]=raw;table=ot.GPOS();table.decompile(OTTableReader(raw,tableTag='GPOS'),font);table.ensureDecompiled(recurse=True)
        def anchor(a):
            totals['anchor_slots']+=1
            if a is None:totals['null_anchors']+=1;return word(0xffffffff)
            totals[f'anchor_format_{a.Format}']+=1
            return word(positions[id(a)])+struct.pack('<HhhH',a.Format,a.XCoordinate,a.YCoordinate,getattr(a,'AnchorPoint',0))+device_blob(getattr(a,'XDeviceTable',None))+device_blob(getattr(a,'YDeviceTable',None))
        out=[]
        for li,l in enumerate(table.LookupList.Lookup):
            for si,wrapped in enumerate(l.SubTable):
                sub=wrapped.ExtSubTable if l.LookupType==9 else wrapped;kind=wrapped.ExtensionLookupType if l.LookupType==9 else l.LookupType
                if kind not in (3,4,5,6):continue
                assert sub.Format==1;totals[f'type_{kind}']+=1;totals['extension_subtables']+=int(l.LookupType==9)
                if kind==3:
                    names=sub.Coverage.glyphs;data=word(li,si,positions[id(sub)],kind,len(names),0,0)
                    for name,row in zip(names,sub.EntryExitRecord):data+=struct.pack('<HH',font.getGlyphID(name),0)+anchor(row.EntryAnchor)+anchor(row.ExitAnchor)
                else:
                    names=(sub.Mark1Coverage if kind==6 else sub.MarkCoverage).glyphs;marks=(sub.Mark1Array if kind==6 else sub.MarkArray).MarkRecord
                    targets=(sub.BaseCoverage if kind==4 else sub.LigatureCoverage if kind==5 else sub.Mark2Coverage).glyphs
                    rows=sub.BaseArray.BaseRecord if kind==4 else sub.LigatureArray.LigatureAttach if kind==5 else sub.Mark2Array.Mark2Record
                    data=word(li,si,positions[id(sub)],kind,len(names),len(targets),sub.ClassCount)
                    for name,row in zip(names,marks):data+=struct.pack('<HH',font.getGlyphID(name),row.Class)+anchor(row.MarkAnchor);totals['marks']+=1
                    for name,row in zip(targets,rows):
                        components=[row.BaseAnchor] if kind==4 else [r.LigatureAnchor for r in row.ComponentRecord] if kind==5 else [row.Mark2Anchor]
                        data+=struct.pack('<HH',font.getGlyphID(name),len(components));totals['targets']+=1;totals['components']+=len(components)
                        for anchors in components:
                            assert len(anchors)==sub.ClassCount
                            data+=b''.join(anchor(a) for a in anchors)
                out.append(data)
        limit=len(font.getGlyphOrder());blob=word(limit,len(raw),len(gd),len(out))+raw+gd+b''.join(out);blobs.append(blob)
        fonts.append({**identity,'subtables':len(out),'gpos_sha256':hashlib.sha256(raw).hexdigest()})
        if identity.get('synthetic'):fixture=b'MCGAT01\0'+word(1)+blob
    for source in json.loads(args.font_manifest.read_text())['fonts']:
        path=Path(source['path']);assert hashlib.sha256(path.read_bytes()).hexdigest()==source['sha256']
        with TTFont(path,fontNumber=source['face']) as font:
            if 'GPOS' in font:emit(font,bytes(font.reader['GPOS']),bytes(font.reader['GDEF']) if 'GDEF' in font else b'',source)
    f,raw,gd=synthetic();emit(f,raw,gd,{'synthetic':True,'name':'first-party attachment arrays, nulls and extensions'})
    args.out.write_bytes(b'MCGAT01\0'+word(len(blobs))+b''.join(blobs))
    metadata=dict(scope='Original attachment data and glyph/class/component queries; not sequence attachment selection or placement parity',fonttools=fontTools.__version__,fonts=fonts,totals=dict(totals),corpus_sha256=hashlib.sha256(args.out.read_bytes()).hexdigest())
    args.out.with_suffix('.json').write_text(json.dumps(metadata,indent=2)+'\n')
    if args.fixture_header:
        write_embedded_array(args.fixture_header,'// First-party offline attachment fixture; generated by gpos_attachment_oracle.py.\n','constexpr u8 attachment_fixture[]',fixture)
    print(json.dumps(dict(fonts=len(blobs),bytes=args.out.stat().st_size,**totals)))
if __name__=='__main__':main()
