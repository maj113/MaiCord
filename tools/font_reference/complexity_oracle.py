"""First-party font participation probes for MaiCord's nominal-mapping gate."""
import argparse,copy,json,subprocess,struct
from pathlib import Path
from fontTools.ttLib import TTFont,newTable
from feature_plan_oracle import layout,language,script,identity
from gsub_execution_oracle import make_font,obj,lookup,single,multiple,ligature,context,context_format,coverage,extension,ORDER
import gpos_execution_oracle as gp

def cases():
    result=[]
    def add(name,kind,lookups,features=None,scripts=None):
        if features is None:features=[('ccmp' if kind=='GSUB' else 'kern',[0])]
        if scripts is None:scripts={'latn':script(language(range(len(features))))}
        result.append(dict(name=name,kind=kind,lookups=lookups,features=features,scripts=scripts))
    add('none','GSUB',[lookup(1,single({'A':'B'}))],[('test',[0])])
    for flags in [0,2,4,8]:
        add('single-'+str(flags),'GSUB',[lookup(1,single({'A':'B'}),flags=flags)])
    for flags in [0,256,512]:
        add('single-attach-'+str(flags),'GSUB',[lookup(1,single({'A':'B'}),flags=flags)])
        for inputs in ['A','AB']:
            for ahead in ['C','M']:
                add(f'chain-attach-{flags}-{inputs}-{ahead}','GSUB',[context(inputs,[(0,1)],flags=flags,chained=True,ahead=ahead),lookup(1,single({'A':'D'}))])
    add('single-identity','GSUB',[lookup(1,single({'A':'A'}))])
    add('multiple','GSUB',[multiple({'A':['B','C']})])
    add('alternate','GSUB',[lookup(3,obj('AlternateSubst',alternates={'A':['B','C']}))])
    add('ligature','GSUB',[ligature('AB')])
    add('extension','GSUB',[extension(ligature('AB'))])
    reverse=lookup(8,obj('ReverseChainSingleSubst',Format=1,Coverage=coverage('A'),BacktrackGlyphCount=1,BacktrackCoverage=[coverage('X')],LookAheadGlyphCount=1,LookAheadCoverage=[coverage('Y')],GlyphCount=1,Substitute=['B']))
    add('reverse','GSUB',[reverse])
    for chained in [False,True]:
        for fmt in [1,2,3]:
            for actions in [[],[(1,1)]]:
                root=context_format('AB',actions,fmt,chained) if fmt!=3 else context('AB',actions,chained=chained,back='X' if chained else (),ahead='Y' if chained else ())
                add(f'context-{int(chained)}-{fmt}-{int(bool(actions))}','GSUB',[root,lookup(1,single({'B':'C','E':'D'}))])
    for tag in ['ccmp','locl','rlig','rvrn','rclt','liga','clig','calt','kern','mark','mkmk','dist','test']:
        for kind in ['GSUB','GPOS']:
            add(kind.lower()+'-tag-'+tag,kind,[lookup(1,single({'A':'B'})) if kind=='GSUB' else gp.adjust('A',13,17,19)],[(tag,[0])])
    for tag in ['test','rclt','ccmp']:
        add('unreferenced-'+tag,'GSUB',[lookup(1,single({'A':'B'}))],[(tag,[0])],{'latn':script(language([]))})
        add('required-'+tag,'GSUB',[lookup(1,single({'A':'B'}))],[(tag,[0])],{'cyrl':script(language([],0))})
    for s in ['DFLT','latn','cyrl']:
        add('script-'+s,'GSUB',[lookup(1,single({'A':'B'}))],scripts={s:script(language([0]))})
    add('named-language','GSUB',[lookup(1,single({'A':'B'}))],scripts={'latn':script(None,{'TRK ':language([0])})})
    add('unreferenced-lookup','GSUB',[lookup(1,single({'A':'B'})),lookup(1,single({'C':'D'}))])
    add('empty-first-duplicate','GSUB',[lookup(1,single({'A':'B'}))],[('ccmp',[]),('ccmp',[0])])
    add('single-zero','GPOS',[gp.adjust('A',0,0,0,0,0)])
    pair=gp.pair();pair.SubTable[0].Coverage=coverage('A');pair.SubTable[0].PairSet=pair.SubTable[0].PairSet[:1];pair.SubTable[0].PairSetCount=1
    pair.SubTable[0].PairSet[0].PairValueRecord=pair.SubTable[0].PairSet[0].PairValueRecord[1:];pair.SubTable[0].PairSet[0].PairValueCount=1
    add('pair','GPOS',[pair]);add('cursive','GPOS',[gp.cursive()]);add('mark-base','GPOS',[gp.mark_base()]);add('mark-lig','GPOS',[gp.mark_lig()]);add('mark-mark','GPOS',[gp.mark_mark()])
    for chained in [False,True]:
        for fmt in [1,2,3]:
            root=gp.position_context_format('AB',[(1,1)],fmt,chained) if fmt!=3 else gp.position_context('AB',[(1,1)],chained=chained,back='X' if chained else (),ahead='Y' if chained else ())
            add(f'position-context-{int(chained)}-{fmt}','GPOS',[root,gp.adjust('E',13)])
    return result

def run(root,exe):
    folder=root/'fixtures';folder.mkdir(parents=True,exist_ok=True);fonts=[];observations=[]
    texts=list('abcdelmnxyz')+['ab','ba','xa','ax','xab','xaby','xay','axy','aaaa','xxaxx','xae','xabe','exab','axbx','xaxbx']+[chr(0x41+i) for i in range(len(ORDER))]
    for c in cases():
        p=folder/(c['name']+'.ttf');make_font(p,dict(name='complexity-'+c['name'],lookups=[lookup(1,single({'A':'A'}))]))
        with TTFont(p,recalcTimestamp=False) as f:
            del f['GSUB'];f[c['kind']]=newTable(c['kind']);f[c['kind']].table=layout(c['kind'],c['features'],c['lookups'],c['scripts'])
            for table in f['cmap'].tables:
                if table.isUnicode():table.cmap.update({ord('m'):'M',ord('n'):'N'} | {0x41+i:g for i,g in enumerate(ORDER)})
            f['GDEF'].table.MarkAttachClassDef=obj('ClassDef',classDefs={'M':1,'N':2})
            f.save(p)
        with TTFont(p,recalcTimestamp=False) as f:f.saveXML(folder/(c['name']+'.ttx'))
        fonts.append(dict(name=c['name'],kind=c['kind'],font=identity(p)))
        for text in texts:
            command=[str(exe.resolve()),str(p.resolve()),','.join(f'{ord(x):x}' for x in text),'default','1148846080','0','0','en-US','layout']
            v=subprocess.run(command,capture_output=True,text=True,timeout=15);assert v.returncode==0,(c['name'],text,v.returncode,v.stdout)
            value=json.loads(v.stdout);assert value['complexity_hresult']==value['layout_hresult']==0
            observations.append(dict(font=c['name'],text=text,result=value))
    (root/'observations.json').write_text(json.dumps(dict(executable=identity(exe),fonts=fonts,cases=observations),indent=2)+'\n')
    summary={f['name']:''.join(c['text'] for c in observations if c['font']==f['name'] and len(c['text'])==1 and not c['result']['text_simple']) for f in fonts}
    (root/'single-glyph-participation.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps(summary,indent=2));print(f'{len(fonts)} fonts, {len(observations)} observations')
def export(root,header):
    data=json.loads((root/'observations.json').read_text());parts=[]
    for f in data['fonts']:
        p=Path(f['font']['path']);assert identity(p)==f['font'];bits=0
        for glyph in range(1,len(ORDER)):
            c=next(c for c in data['cases'] if c['font']==f['name'] and c['text']==chr(0x41+glyph))
            assert c['result']['complexity_length']==1
            if not c['result']['text_simple']:bits|=1<<glyph
        parts.append(struct.pack('<IIIQ',p.stat().st_size,int(f['kind']=='GPOS'),len(ORDER),bits)+p.read_bytes())
    raw=b'MCNOM01\0'+struct.pack('<I',len(parts))+b''.join(parts);(root/'corpus.bin').write_bytes(raw)
    header.write_text('// First-party fonts and Windows font-participation observations; glyph zero excluded.\nconstexpr u8 participation_fixture[]{\n'+'\n'.join('    '+','.join(f'0x{x:02x}' for x in raw[i:i+24])+',' for i in range(0,len(raw),24))+'\n};\n')
    print(f'Exported {len(parts)} fonts, {len(raw)} bytes')
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('root',type=Path);p.add_argument('--exe',type=Path);p.add_argument('--header',type=Path);a=p.parse_args()
    if a.exe:run(a.root,a.exe)
    if a.header:export(a.root,a.header)
