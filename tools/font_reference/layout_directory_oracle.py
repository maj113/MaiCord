"""Capture original GSUB/GPOS directories and fontTools expectations (offline QA)."""
import argparse,copy,hashlib,itertools,json,struct
from types import SimpleNamespace
from pathlib import Path
import fontTools
from fontTools.ttLib import TTFont
from fontTools.ttLib.tables import otTables
from fontTools.ttLib.tables.otBase import BaseTable,OTTableReader
from fontTools.varLib.instancer import NormalizedAxisLimits
from fontTools.varLib.instancer.featureVars import _instantiateFeatureVariations

def synthetic(kind,compatibility_edges=True):
    w=lambda *v:struct.pack('>'+'H'*len(v),*v)
    d=lambda *v:struct.pack('>'+'I'*len(v),*v)
    def tags(entries):
        b=bytearray(w(len(entries))+bytes(6*len(entries)))
        for i,(tag,value) in enumerate(entries):struct.pack_into('>4sH',b,2+i*6,tag.encode(),len(b));b+=value
        return b
    def script(default,languages):
        b=bytearray(w(0,len(languages))+bytes(6*len(languages)))
        if default is not None:struct.pack_into('>H',b,0,len(b));b+=default
        for i,(tag,value) in enumerate(languages):struct.pack_into('>4sH',b,4+i*6,tag.encode(),len(b));b+=value
        return b
    def feature(indices,params=b''):return w(4+2*len(indices) if params else 0,len(indices),*indices)+params
    scripts=tags([('DFLT',script(w(0,1,2,1,0),[])),('latn',script(w(0,65535,1,0),[('TRK ',w(0,65535,1,1))]))])
    features=tags([('liga',feature([0])),('liga',feature([1,0])),('ss01',feature([],w(0,256)))])
    payload=w(1,6,1,1,1,1) if kind==0 else w(1,8,1,10,1,1,1)
    normal=w(1,0,1,8)+payload;extension=w(1,1)+d(8)+payload
    wrapped=w(7 if kind==0 else 9,0x210,2,12,12+len(extension),1)+extension+extension
    lookups=w(2,6,6+len(normal))+normal+wrapped
    def conditions(values):
        return w(len(values))+d(*(2+4*len(values)+8*i for i in range(len(values))))+b''.join(w(1,a,lo&65535,hi&65535) for a,lo,hi in values)
    def substitutions(values,version=1):
        b=bytearray(w(version,0,len(values))+bytes(6*len(values)))
        for i,(index,indices) in enumerate(values):struct.pack_into('>HI',b,6+i*6,index,len(b));b+=feature(indices)
        return b
    records=[(conditions([(3,-16384,16384)]),substitutions([(0,[1])])),
             (conditions([(0,-8192,8192)]),substitutions([(0,[1])],version=2)),
             (conditions([(0,0,8192),(1,-8192,8192)]),substitutions([(0,[1])])),
             (conditions([(0,-8192,8192)]),substitutions([(1,[]),(2,[0])])),
             (None,None),(None,substitutions([(0,[1])]))]
    if not compatibility_edges:records=records[2:4]+[(None,substitutions([]))]
    variations=bytearray(w(1,0)+d(len(records))+bytes(8*len(records)))
    for i,pair in enumerate(records):
        for j,value in enumerate(pair):
            if value is not None:struct.pack_into('>I',variations,8+i*8+j*4,len(variations));variations+=value
    b=bytearray(w(1,1)+bytes(10))
    for field,value in [(4,scripts),(6,features),(8,lookups),(10,variations)]:struct.pack_into('>I' if field==10 else '>H',b,field,len(b));b+=value
    return bytes(b)

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--font-manifest',required=True,type=Path);p.add_argument('--out',required=True,type=Path);p.add_argument('--fixture-header',type=Path);a=p.parse_args();a.out.parent.mkdir(parents=True,exist_ok=True)
    positions={};active_raw=[None];original=BaseTable.decompile
    def observed(self,reader,font):
        assert bytes(reader.data)==active_raw[0]
        positions[id(self)]=reader.pos
        original(self,reader,font)
    BaseTable.decompile=observed
    w=lambda *v:struct.pack('<'+'I'*len(v),*(x&0xffffffff for x in v))
    tag=lambda s:int.from_bytes(s.encode(),'big')
    records=[];metadata=[]
    def emit(raw,t,kind,axes,identity):
        scripts=t.ScriptList.ScriptRecord;features=t.FeatureList.FeatureRecord;lookups=t.LookupList.Lookup
        b=bytearray(w(kind,len(raw),axes)+raw+w(len(scripts)));counts={'scripts':len(scripts),'languages':0,'features':len(features),'lookups':len(lookups),'subtables':0,'extension_subtables':0}
        for sr in scripts:
            s=sr.Script;langs=([(0xffffffff,0,s.DefaultLangSys)] if s.DefaultLangSys else [])+[(i,tag(l.LangSysTag),l.LangSys) for i,l in enumerate(s.LangSysRecord)]
            b+=w(tag(sr.ScriptTag),positions[id(s)],s.LangSysCount,len(langs));counts['languages']+=len(langs)
            for index,ltag,lang in langs:b+=w(index,ltag,lang.ReqFeatureIndex,len(lang.FeatureIndex),*lang.FeatureIndex)
        b+=w(len(features))
        for f in features:
            obj=f.Feature;params=obj.FeatureParams
            b+=w(tag(f.FeatureTag),positions[id(obj)],positions[id(params)] if params else 0xffffffff,len(obj.LookupListIndex),*obj.LookupListIndex)
        b+=w(len(lookups))
        for lookup in lookups:
            b+=w(lookup.LookupType,lookup.LookupFlag,getattr(lookup,'MarkFilteringSet',0),len(lookup.SubTable))
            for sub in lookup.SubTable:
                ext=lookup.LookupType==(7 if kind==0 else 9);resolved=sub.ExtSubTable if ext else sub;typ=sub.ExtensionLookupType if ext else lookup.LookupType
                at=positions[id(resolved)];assert 0<=at<=len(raw)-2
                b+=w(typ,at);counts['subtables']+=1;counts['extension_subtables']+=ext
        fv=getattr(t,'FeatureVariations',None);samples=[(0,)*axes]
        if identity.get('synthetic'):samples=list(itertools.product([-16384,-8193,-8192,-1,0,8192,8193,16383,16384],repeat=axes))
        b+=w(len(samples))
        for coords in samples:
            selected=0xffffffff;replacement={}
            if fv:
                for i,rec in enumerate(fv.FeatureVariationRecord):
                    conditions=rec.ConditionSet.ConditionTable if rec.ConditionSet else []
                    if not all(c.Format==1 and c.AxisIndex<len(coords) and c.FilterRangeMinValue<=coords[c.AxisIndex]/16384<=c.FilterRangeMaxValue for c in conditions):continue
                    sub=rec.FeatureTableSubstitution
                    if sub and sub.Version!=0x10000:continue
                    selected=i;replacement={r.FeatureIndex:r.Feature for r in sub.SubstitutionRecord} if sub else {};break
            b+=w(axes,*coords,selected,len(replacement))
            if identity.get('instancer_checked'):
                independent=copy.deepcopy(t);axis_tags=['wght','wdth'];limits=NormalizedAxisLimits({name:value/16384 for name,value in zip(axis_tags,coords)})
                _instantiateFeatureVariations(independent,[SimpleNamespace(axisTag=name) for name in axis_tags],limits)
                assert [f.Feature.LookupListIndex for f in independent.FeatureList.FeatureRecord]==[replacement.get(i,f.Feature).LookupListIndex for i,f in enumerate(features)]
            for i,f in enumerate(features):
                value=replacement.get(i,f.Feature);b+=w(len(value.LookupListIndex),*value.LookupListIndex)
        records.append(b);metadata.append({**identity,'kind':'GSUB' if kind==0 else 'GPOS','raw_sha256':hashlib.sha256(raw).hexdigest(),'axes':axes,**counts,'variation_records':fv.FeatureVariationCount if fv else 0,'variation_probes':len(samples)})
    for source in json.loads(a.font_manifest.read_text())['fonts']:
        assert hashlib.sha256(Path(source['path']).read_bytes()).hexdigest()==source['sha256']
        with TTFont(source['path'],fontNumber=source['face']) as font:
            assert len(font.getGlyphOrder())==font['maxp'].numGlyphs==source['glyphs']
            for kind,name in enumerate(['GSUB','GPOS']):
                if name not in font:continue
                positions.clear();raw=bytes(font.reader[name]);active_raw[0]=raw;t=font[name].table;t.ensureDecompiled(recurse=True);emit(raw,t,kind,len(font['fvar'].axes) if 'fvar' in font else 0,source)
    for kind,edges in itertools.product(range(2),[True,False]):
        positions.clear();font=TTFont();font.setGlyphOrder(['g0','g1','g2']);raw=synthetic(kind,edges);active_raw[0]=raw;name='GSUB' if kind==0 else 'GPOS';t=getattr(otTables,name)();t.decompile(OTTableReader(raw,tableTag=name),font);t.ensureDecompiled(recurse=True);emit(raw,t,kind,2,{'synthetic':True,'name':name+('-compatibility' if edges else '-instancer'),'instancer_checked':not edges})
    a.out.write_bytes(b'MCLDIR1\0'+w(len(records))+b''.join(records));meta={'fontTools_version':fontTools.__version__,'records':metadata,'corpus_sha256':hashlib.sha256(a.out.read_bytes()).hexdigest()};a.out.with_suffix('.json').write_text(json.dumps(meta,indent=2)+'\n')
    if a.fixture_header:
        raw=synthetic(0);a.fixture_header.parent.mkdir(parents=True,exist_ok=True);a.fixture_header.write_text('// Generated by tools/font_reference/layout_directory_oracle.py; QA-only.\nstatic constexpr u8 layout_directory_fixture[]{\n'+''.join('    '+','.join(str(v) for v in raw[i:i+24])+',\n' for i in range(0,len(raw),24))+'};\n')
    print(json.dumps({'records':len(records),'totals':{k:sum(r[k] for r in metadata) for k in ['scripts','languages','features','lookups','subtables','extension_subtables','variation_probes']}}))

if __name__=='__main__':main()
