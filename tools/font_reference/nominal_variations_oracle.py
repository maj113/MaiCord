"""Measure required normal/semibold feature-variation behavior; QA only."""
import argparse,json,subprocess,struct
from pathlib import Path
from fontTools.ttLib import TTFont,newTable
from fontTools.ttLib.tables._f_v_a_r import Axis
from feature_plan_oracle import layout,language,script,identity
from gsub_execution_oracle import make_font,lookup,single,obj
from embed_fixture import write_embedded_array
from gpos_execution_oracle import adjust

def generate(folder):
    folder.mkdir(parents=True,exist_ok=True);records=[]
    for kind,tags in [('GSUB',['ccmp','rvrn','liga','test']),('GPOS',['kern','mark','test'])]:
        for tag in tags:
            p=folder/f'{kind.lower()}-{tag}.ttf';make_font(p,dict(name=f'variable-{kind}-{tag}',lookups=[lookup(1,single({'A':'A'}))]))
            roots=[lookup(1,single({'A':'B'})),lookup(1,single({'C':'D'}))] if kind=='GSUB' else [adjust('A',13,17,19),adjust('C',23,29,31)]
            langs=language([],0) if tag=='test' else language([0]);t=layout(kind,[(tag,[0])],roots,{'latn':script(langs)});t.Version=0x10001
            replacement=obj('Feature',FeatureParams=None,LookupCount=1,LookupListIndex=[1]);sub=obj('FeatureTableSubstitution',Version=0x10000,SubstitutionCount=1,SubstitutionRecord=[obj('FeatureTableSubstitutionRecord',FeatureIndex=0,Feature=replacement)])
            conditions=obj('ConditionSet',ConditionCount=1,ConditionTable=[obj('ConditionTable',Format=1,AxisIndex=0,FilterRangeMinValue=.25,FilterRangeMaxValue=1.)])
            t.FeatureVariations=obj('FeatureVariations',Version=0x10000,FeatureVariationCount=1,FeatureVariationRecord=[obj('FeatureVariationRecord',ConditionSet=conditions,FeatureTableSubstitution=sub)])
            with TTFont(p,recalcTimestamp=False) as f:
                del f['GSUB'];f[kind]=newTable(kind);f[kind].table=t
                f['fvar']=newTable('fvar');axis=Axis();axis.axisTag='wght';axis.minValue=100;axis.defaultValue=400;axis.maxValue=900;axis.flags=0;axis.axisNameID=256;f['fvar'].axes=[axis];f['fvar'].instances=[];f['name'].setName('Weight',256,3,1,0x409)
                f['gvar']=newTable('gvar');f['gvar'].version=1;f['gvar'].reserved=0;f['gvar'].variations={g:[] for g in f.getGlyphOrder()};f.save(p)
            with TTFont(p,recalcTimestamp=False) as f:f.saveXML(p.with_suffix('.ttx'))
            records.append(dict(name=p.stem,kind=kind,font=identity(p)))
    return records
def run(root,exe):
    fonts=generate(root/'fixtures');observations=[]
    for font in fonts:
        for weight in [400,600]:
            for text in ['a','c','ac']:
                for mode in ['default','liga=0,clig=0,kern=0']:
                    command=[str(exe.resolve()),font['font']['path'],','.join(f'{ord(c):x}' for c in text),mode,'1148846080','0','0','en-US','layout',str(weight)]
                    r=subprocess.run(command,capture_output=True,text=True,timeout=20);assert r.returncode==0,(command,r.stdout,r.stderr);value=json.loads(r.stdout)
                    weight_bits=struct.unpack('<I',struct.pack('<f',weight))[0];assert value['face_weight_bits']==value['layout_weight_bits']==weight_bits
                    observations.append(dict(font=font['name'],weight=weight,text=text,mode=mode,result=value))
    root.mkdir(parents=True,exist_ok=True);(root/'observations.json').write_text(json.dumps(dict(reference=identity(exe),fonts=fonts,cases=observations),indent=2)+'\n')
    print(json.dumps([dict(font=c['font'],weight=c['weight'],text=c['text'],simple=c['result']['text_simple'],full=c['result']['glyphs'],layout=c['result']['layout_glyphs']) for c in observations if c['mode']=='default' and c['text']!='ac'],indent=2))
def export(root,header):
    data=json.loads((root/'observations.json').read_text());w=lambda *xs:struct.pack('<'+'I'*len(xs),*xs);parts=[]
    for font in data['fonts']:
        p=Path(font['font']['path']);assert identity(p)==font['font'];cases=[c for c in data['cases'] if c['font']==font['name']];blob=w(p.stat().st_size,int(font['kind']=='GPOS'),len(cases))+p.read_bytes()
        for c in cases:
            v=c['result'];n=len(c['text']);assert len(v['glyphs'])==len(v['layout_glyphs'])==n
            blob+=w(c['weight'],int(c['mode']!='default'),n,0 if c['weight']==600 else 0xffffffff,v['text_simple'])
            blob+=w(*map(ord,c['text']),*v['glyphs'],*[x for row in v['placement_bits'] for x in row],*v['layout_glyphs'],*[x for row in v['layout_placement_bits'] for x in row])
        parts.append(blob)
    raw=b'MCNVAR1\0'+w(len(parts))+b''.join(parts);(root/'corpus.bin').write_bytes(raw);write_embedded_array(header,'// First-party variable fonts and Windows normal/semibold expectations.\n','constexpr u8 nominal_variations_fixture[]',raw);print(f'Exported {len(parts)} fonts, {len(data["cases"])} cases, {len(raw)} bytes')
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('root',type=Path);p.add_argument('--exe',type=Path);p.add_argument('--header',type=Path);a=p.parse_args()
    if a.exe:run(a.root,a.exe)
    if a.header:export(a.root,a.header)
