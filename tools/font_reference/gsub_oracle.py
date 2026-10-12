"""Original-font GSUB 1..4 data and independent match probes; offline QA only."""
import argparse,collections,hashlib,json,struct
from pathlib import Path
import fontTools
from fontTools.ttLib import TTFont
from fontTools.ttLib.tables.otBase import BaseTable

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--font-manifest',required=True,type=Path);p.add_argument('--out',required=True,type=Path);a=p.parse_args();a.out.parent.mkdir(parents=True,exist_ok=True)
    positions={};active=[None];original=BaseTable.decompile
    def observed(self,reader,font):
        if str(reader.tableTag)=='GSUB':assert bytes(reader.data)==active[0];positions[id(self)]=reader.pos
        original(self,reader,font)
    BaseTable.decompile=observed
    w=lambda *v:struct.pack('<'+'I'*len(v),*v)
    records=[];metadata=[];totals=collections.Counter()
    for source in json.loads(a.font_manifest.read_text())['fonts']:
        assert hashlib.sha256(Path(source['path']).read_bytes()).hexdigest()==source['sha256']
        with TTFont(source['path'],fontNumber=source['face']) as font:
            if 'GSUB' not in font:continue
            limit=len(font.getGlyphOrder());assert limit==font['maxp'].numGlyphs==source['glyphs'];positions.clear();raw=bytes(font.reader['GSUB']);active[0]=raw;t=font['GSUB'].table;t.ensureDecompiled(recurse=True)
            gdef_raw=bytes(font.reader['GDEF']) if 'GDEF' in font else b'';gdef=font['GDEF'].table if gdef_raw else None;classes=[0]*limit;mark_classes=[0]*limit
            if gdef and gdef.GlyphClassDef:
                for name,value in gdef.GlyphClassDef.classDefs.items():classes[font.getGlyphID(name)]=value
            if gdef and gdef.MarkAttachClassDef:
                for name,value in gdef.MarkAttachClassDef.classDefs.items():mark_classes[font.getGlyphID(name)]=value
            sets=getattr(gdef,'MarkGlyphSetsDef',None);sets=[{font.getGlyphID(name) for name in c.glyphs} for c in sets.Coverage] if sets else []
            subtables=[];submeta=[]
            for li,lookup in enumerate(t.LookupList.Lookup):
                for si,encoded in enumerate(lookup.SubTable):
                    typ=encoded.ExtensionLookupType if lookup.LookupType==7 else lookup.LookupType;sub=encoded.ExtSubTable if lookup.LookupType==7 else encoded
                    if typ not in (1,2,3,4):continue
                    flags=lookup.LookupFlag;markset=getattr(lookup,'MarkFilteringSet',0)
                    if typ==4:mapping={font.getGlyphID(name):[([font.getGlyphID(x) for x in lig.Component],font.getGlyphID(lig.LigGlyph)) for lig in ligs] for name,ligs in sub.ligatures.items()}
                    else:
                        data=sub.alternates if typ==3 else sub.mapping
                        mapping={font.getGlyphID(name):[font.getGlyphID(x) for x in ([value] if typ==1 else value)] for name,value in data.items()}
                    b=bytearray(w(typ,positions[id(sub)],li,si,flags,markset,len(mapping)))
                    for gid,value in sorted(mapping.items()):
                        b+=w(gid)
                        if typ!=4:b+=w(len(value),*value)
                    def ignored(gid):
                        cl=classes[gid]
                        if cl==1:return bool(flags&2)
                        if cl==2:return bool(flags&4)
                        if cl!=3:return False
                        if flags&8:return True
                        if flags&16:return gid not in sets[markset]
                        return bool(flags>>8 and mark_classes[gid]!=(flags>>8))
                    cases=[];seen=set()
                    def probe(sequence,start=0):
                        key=(tuple(sequence),start)
                        if key in seen:return
                        seen.add(key);consumed=[];output=[]
                        for rest,replacement in mapping.get(sequence[start],[]):
                            eligible=[at for at in range(start+1,len(sequence)) if not ignored(sequence[at])]
                            if len(eligible)>=len(rest) and [sequence[at] for at in eligible[:len(rest)]]==rest:
                                consumed=[start]+eligible[:len(rest)];output=[replacement];break
                        cases.append(w(start,len(sequence),len(consumed),len(output))+b''.join(w(gid,classes[gid]) for gid in sequence)+w(*consumed,*output))
                    if typ==4:
                        marks=[gid for gid,cl in enumerate(classes) if cl==3];representatives=[]
                        for wanted in (False,True):
                            found=next((gid for gid in marks if ignored(gid)==wanted),None)
                            if found is not None:representatives.append(found)
                        for first,ligs in mapping.items():
                            for rest,replacement in ligs:
                                seq=[first]+rest;probe(seq);probe([0]+seq,1);probe(seq+[0]);probe(seq[:-1] or seq)
                                probe(seq[:-1]+[(seq[-1]+1)%limit])
                                for mark in representatives:probe([first,mark]+rest)
                                totals['ligature_rules']+=1
                    fmt=int.from_bytes(raw[positions[id(sub)]:positions[id(sub)]+2],'big')
                    b+=w(len(cases))+b''.join(cases);subtables.append(b);totals[f'type_{typ}']+=1;totals[f'format_{typ}_{fmt}']+=1;totals['mappings']+=len(mapping);totals['ligature_probes']+=len(cases)
                    submeta.append({'lookup':li,'subtable':si,'type':typ,'format':fmt,'offset':positions[id(sub)],'mappings':len(mapping),'probes':len(cases)})
            records.append(w(limit,len(raw),len(gdef_raw),len(subtables))+raw+gdef_raw+b''.join(subtables));metadata.append({**source,'gsub_sha256':hashlib.sha256(raw).hexdigest(),'gdef_sha256':hashlib.sha256(gdef_raw).hexdigest(),'subtables':submeta})
    a.out.write_bytes(b'MCGSUB1\0'+w(len(records))+b''.join(records));meta={'fontTools_version':fontTools.__version__,'fonts':metadata,'totals':dict(totals),'corpus_sha256':hashlib.sha256(a.out.read_bytes()).hexdigest()};a.out.with_suffix('.json').write_text(json.dumps(meta,indent=2)+'\n');print(json.dumps({'fonts':len(records),**totals,'bytes':a.out.stat().st_size}))

if __name__=='__main__':main()
