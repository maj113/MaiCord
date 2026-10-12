"""Capture GSUB/GPOS contextual patterns and independent match/action probes."""
import argparse,collections,hashlib,json,struct
from pathlib import Path
import fontTools
from fontTools.ttLib import TTFont
from fontTools.ttLib.tables import otTables
from fontTools.ttLib.tables.otBase import BaseTable,OTTableReader

def synthetic(mode,fmt=1):
    w=lambda *v:struct.pack('>'+'H'*len(v),*v)
    cov=lambda ids:w(1,len(ids),*ids)
    actions=w(1,2,0,1,1,0)
    if mode==2:
        b=bytearray(w(1,0,2,0,0,2,0,0,2,8,4));fields=[(2,[3,7]),(6,[1]),(8,[2]),(12,[5]),(14,[6])]
    elif fmt==3:
        if mode==1:b=bytearray(w(3,2,0,0,2,0,0,2,0,0,3)+actions);fields=[(4,[1]),(6,[2]),(10,[3,7]),(12,[4,8]),(16,[5]),(18,[6])]
        else:b=bytearray(w(3,2,3,0,0)+actions);fields=[(6,[3,7]),(8,[4,8])]
    else:
        rest=0 if fmt==2 else 4
        full=(w(2,1,2,2,rest,2,1,2,3) if fmt==2 else w(2,1,2,2,rest,2,5,6,3))+actions if mode else w(2,3,rest)+actions
        empty=w(0,1,0,0) if mode else w(1,0)
        rules=w(2,6,6+len(full))+full+empty
        if fmt==1:b=bytearray(w(1,0,1,8)+rules);fields=[(2,[3])]
        else:
            header=w(2,0,0,0,0,2,0,16) if mode else w(2,0,0,2,0,12)
            b=bytearray(header+rules);fields=[(2,[3,7])]
            class_values=[(4,w(1,1,2,1,2)),(6,w(1,3,5,1,0,0,0,1)),(8,w(1,5,2,1,2))] if mode else [(4,w(1,3,5,1,0,0,0,1))]
            for field,value in class_values:struct.pack_into('>H',b,field,len(b));b+=value
    for field,ids in fields:struct.pack_into('>H',b,field,len(b));b+=cov(ids)
    return bytes(b)

def gdef_fixture():
    return struct.pack('>19H',1,0,12,0,0,0,1,0,10,0,1,1,3,1,1,1,1,1,3)

def normalized(sub,mode,font):
    gid=font.getGlyphID;limit=len(font.getGlyphOrder());actions=lambda obj:[(a.SequenceIndex,a.LookupListIndex) for a in getattr(obj,'SubstLookupRecord',getattr(obj,'PosLookupRecord',[]))]
    coverage=lambda obj:set(gid(name) for name in obj.glyphs)
    if mode==2:
        mapping=dict(zip(sub.Coverage.glyphs,sub.Substitute));return [(coverage(sub.Coverage),[],[coverage(c) for c in sub.BacktrackCoverage],[coverage(c) for c in sub.LookAheadCoverage],None,{gid(k):gid(v) for k,v in mapping.items()})]
    if sub.Format==3:
        inputs=sub.InputCoverage if mode else sub.Coverage;return [(coverage(inputs[0]),[coverage(c) for c in inputs[1:]],[coverage(c) for c in sub.BacktrackCoverage] if mode else [],[coverage(c) for c in sub.LookAheadCoverage] if mode else [],actions(sub),None)]
    def classes(obj):
        buckets=collections.defaultdict(set);assigned={gid(k):v for k,v in obj.classDefs.items()} if obj else {}
        for g in range(limit):buckets[assigned.get(g,0)].add(g)
        return buckets
    firsts=coverage(sub.Coverage);rules=[];pos='Pos' if hasattr(sub,'PosRuleSet') or hasattr(sub,'PosClassSet') or hasattr(sub,'ChainPosRuleSet') or hasattr(sub,'ChainPosClassSet') else 'Sub';prefix='Chain' if mode else ''
    if sub.Format==1:
        sets=getattr(sub,prefix+pos+'RuleSet');starts=[[gid(name)] for name in sub.Coverage.glyphs];class_maps=None
    else:
        sets=getattr(sub,prefix+pos+'ClassSet');class_maps=[classes(sub.BacktrackClassDef),classes(sub.InputClassDef),classes(sub.LookAheadClassDef)] if mode else [None,classes(sub.ClassDef),None];starts=[firsts&class_maps[1][i] for i in range(len(sets))]
    for start,rule_set in zip(starts,sets):
        if rule_set is None:continue
        for rule in getattr(rule_set,prefix+pos+('Rule' if sub.Format==1 else 'ClassRule')):
            values=lambda data,part:[{gid(v)} for v in data] if sub.Format==1 else [class_maps[part][v] for v in data]
            inputs=rule.Class if sub.Format==2 and not mode else rule.Input
            rules.append((set(start),values(inputs,1),values(rule.Backtrack,0) if mode else [],values(rule.LookAhead,2) if mode else [],actions(rule),None))
    return rules

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--font-manifest',required=True,type=Path);p.add_argument('--out',required=True,type=Path);p.add_argument('--fixture-header',type=Path);a=p.parse_args();a.out.parent.mkdir(parents=True,exist_ok=True)
    positions={};active=[None];original=BaseTable.decompile
    def observed(self,reader,font):
        if str(reader.tableTag) in ('GSUB','GPOS'):assert bytes(reader.data)==active[0];positions[id(self)]=reader.pos
        original(self,reader,font)
    BaseTable.decompile=observed;w=lambda *v:struct.pack('<'+'I'*len(v),*v);records=[];metadata=[];totals=collections.Counter()
    def emit(font,raw,graw,kind,lookup_limit,entries,identity):
        limit=len(font.getGlyphOrder());g=font['GDEF'].table if 'GDEF' in font else None;cl=[0]*limit;mc=[0]*limit
        if g and g.GlyphClassDef:
            for name,v in g.GlyphClassDef.classDefs.items():cl[font.getGlyphID(name)]=v
        if g and g.MarkAttachClassDef:
            for name,v in g.MarkAttachClassDef.classDefs.items():mc[font.getGlyphID(name)]=v
        sets=getattr(g,'MarkGlyphSetsDef',None);sets=[{font.getGlyphID(n) for n in c.glyphs} for c in sets.Coverage] if sets else []
        blobs=[];submeta=[]
        for sub,mode,offset,flags,markset,li,si in entries:
            patterns=normalized(sub,mode,font)
            def ignored(gid):
                if cl[gid]==1:return bool(flags&2)
                if cl[gid]==2:return bool(flags&4)
                if cl[gid]!=3:return False
                if flags&8:return True
                if flags&16:return gid not in sets[markset]
                return bool(flags>>8 and mc[gid]!=(flags>>8))
            eligible={gid for gid in range(limit) if not ignored(gid)};seen=set();probes=[];positive=0
            def probe(seq,start,end):
                nonlocal positive
                key=(tuple(seq),start,end)
                if key in seen:return
                seen.add(key);matched=[];acts=[]
                for first,rest,back,look,actions,replacements in patterns:
                    if seq[start] not in first:continue
                    inputs=[i for i in range(start+1,end) if not ignored(seq[i])]
                    if len(inputs)<len(rest) or any(seq[i] not in allowed for i,allowed in zip(inputs,rest)):continue
                    consumed=[start]+inputs[:len(rest)];before=[i for i in range(start-1,-1,-1) if not ignored(seq[i])];after=[i for i in range(consumed[-1]+1,len(seq)) if not ignored(seq[i])]
                    if len(before)<len(back) or len(after)<len(look) or any(seq[i] not in allowed for i,allowed in zip(before,back)) or any(seq[i] not in allowed for i,allowed in zip(after,look)):continue
                    matched=consumed;acts=actions if mode!=2 else [(0,replacements[seq[start]])];break
                positive+=bool(matched);probes.append(w(start,end,len(seq),len(matched),len(acts))+b''.join(w(gid,cl[gid]) for gid in seq)+w(*matched)+b''.join(w(*pair) for pair in acts))
            skipped=next((g for g in range(limit) if ignored(g)),None)
            for first,rest,back,look,actions,replacements in patterns:
                if not first:continue
                parts=[list(reversed(back)),[first]+rest,look];flat=sum(parts,[])
                for sample in range(3):
                    picked=[]
                    for index,values in enumerate(flat):
                        choices=sorted(values if index==len(back) else values&eligible)
                        if not choices:choices=sorted(values)
                        if not choices:break
                        picked.append(choices[0 if sample==0 else -1 if sample==1 else len(choices)//2])
                    if len(picked)!=len(flat):continue
                    start=len(back);end=start+1+len(rest);probe(picked,start,end);probe([0]+picked,start+1,end+1);probe(picked+[0],start,end)
                    if rest:probe(picked,start,end-1)
                    for index in (0,start,len(picked)-1):
                        missing=next((g for g in range(limit) if g not in flat[index] and g in eligible),None)
                        if missing is not None:changed=picked.copy();changed[index]=missing;probe(changed,start,end)
                    if skipped is not None and len(picked)>1:
                        injected=[]
                        for i,glyph in enumerate(picked):
                            if i:injected.append(skipped)
                            injected.append(glyph)
                        probe(injected,start*2,end*2-1)
            max_inputs=max((1+len(rule[1]) for rule in patterns),default=0)
            coverage_count=1+len(patterns[0][2])+len(patterns[0][3]) if mode==2 else 1+len(patterns[0][1])+len(patterns[0][2])+len(patterns[0][3]) if sub.Format==3 else 1
            class_count=(3 if mode else 1) if mode!=2 and sub.Format==2 else 0
            blobs.append(w(mode,offset,flags,markset,li,si,coverage_count,class_count,max_inputs,len(probes))+b''.join(probes));submeta.append({'mode':mode,'format':sub.Format,'offset':offset,'lookup':li,'subtable':si,'rules':len(patterns),'probes':len(probes),'positive':positive});totals[f'mode_{mode}_format_{sub.Format}']+=1;totals['rules']+=len(patterns);totals['probes']+=len(probes);totals['positive']+=positive
        records.append(w(limit,len(raw),len(graw),len(blobs),kind,lookup_limit)+raw+graw+b''.join(blobs));metadata.append({**identity,'kind':kind,'raw_sha256':hashlib.sha256(raw).hexdigest(),'gdef_sha256':hashlib.sha256(graw).hexdigest(),'subtables':submeta})
    scanned_fonts=json.loads(a.font_manifest.read_text())['fonts']
    for source in scanned_fonts:
        assert hashlib.sha256(Path(source['path']).read_bytes()).hexdigest()==source['sha256']
        with TTFont(source['path'],fontNumber=source['face']) as font:
            assert len(font.getGlyphOrder())==font['maxp'].numGlyphs==source['glyphs']
            for kind,tag in enumerate(['GSUB','GPOS']):
                if tag not in font:continue
                positions.clear();raw=bytes(font.reader[tag]);active[0]=raw;t=font[tag].table;t.ensureDecompiled(recurse=True);entries=[]
                for li,l in enumerate(t.LookupList.Lookup):
                    for si,s in enumerate(l.SubTable):
                        typ=s.ExtensionLookupType if l.LookupType==(7 if kind==0 else 9) else l.LookupType;s=s.ExtSubTable if l.LookupType==(7 if kind==0 else 9) else s
                        if typ not in ((5,6,8) if kind==0 else (7,8)):continue
                        mode=2 if kind==0 and typ==8 else int(typ==(6 if kind==0 else 8));entries.append((s,mode,positions[id(s)],l.LookupFlag,getattr(l,'MarkFilteringSet',0),li,si))
                if entries:emit(font,raw,bytes(font.reader['GDEF']) if 'GDEF' in font else b'',kind,t.LookupList.LookupCount,entries,source)
    fixtures=[]
    for mode,fmt in [(m,f) for m in range(2) for f in (1,2,3)]+[(2,1)]:
        font=TTFont();font.setGlyphOrder([f'g{i}' for i in range(10)]);from fontTools.ttLib import newTable
        gd=gdef_fixture();gt=newTable('GDEF');gt.decompile(gd,font);font['GDEF']=gt;raw=synthetic(mode,fmt);active[0]=raw;positions.clear();sub=getattr(otTables,['ContextSubst','ChainContextSubst','ReverseChainSingleSubst'][mode])();sub.decompile(OTTableReader(raw,tableTag='GSUB'),font);sub.ensureDecompiled(recurse=True)
        emit(font,raw,gd,2,3,[(sub,mode,0,8,0,0xffffffff,0)],{'synthetic':True,'name':f'context-{mode}-{fmt}'});fixtures.append((mode,fmt,raw))
    a.out.write_bytes(b'MCCTX1\0\0'+w(len(records))+b''.join(records));meta={'fontTools_version':fontTools.__version__,'scanned_fonts':scanned_fonts,'fonts':metadata,'totals':dict(totals),'corpus_sha256':hashlib.sha256(a.out.read_bytes()).hexdigest()};a.out.with_suffix('.json').write_text(json.dumps(meta,indent=2)+'\n')
    if a.fixture_header:
        def array(name,b):return 'static constexpr u8 '+name+'[]{\n'+''.join('    '+','.join(str(v) for v in b[i:i+24])+',\n' for i in range(0,len(b),24))+'};\n'
        content='// Generated by tools/font_reference/context_oracle.py; QA-only.\n'+array('context_gdef_fixture',gdef_fixture())
        for mode,fmt,raw in fixtures:content+=array(f'context_{mode}_{fmt}',raw)
        content+='struct ContextFixture {const u8* data;u32 size;u16 mode,format;};\nstatic constexpr ContextFixture context_fixtures[]{\n'+''.join(f'    {{context_{m}_{f},sizeof(context_{m}_{f}),{m},{f}}},\n' for m,f,_ in fixtures)+'};\n';a.fixture_header.parent.mkdir(parents=True,exist_ok=True);a.fixture_header.write_text(content)
    print(json.dumps({'tables':len(records),**totals,'bytes':a.out.stat().st_size}))

if __name__=='__main__':main()
