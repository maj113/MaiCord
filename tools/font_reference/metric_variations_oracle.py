"""First-party HVAR/MVAR fixtures and required-weight Windows metrics; QA only."""
import argparse,json,struct,subprocess
from pathlib import Path
from fontTools.ttLib import TTFont,newTable
from fontTools.ttLib.tables._f_v_a_r import Axis
from fontTools.varLib.builder import buildVarRegionList,buildVarData,buildVarStore
from gsub_execution_oracle import make_font,lookup,single,obj,ORDER
from feature_plan_oracle import identity

def generate(folder):
    folder.mkdir(parents=True,exist_ok=True);files=[]
    for mode in ['implicit','mapped','negative','fractional','half','empty']:
        p=folder/(mode+'.ttf');make_font(p,dict(name='metric-'+mode,lookups=[lookup(1,single({'A':'A'}))]))
        with TTFont(p,recalcTimestamp=False) as f:
            del f['GSUB'];f['fvar']=newTable('fvar');a=Axis();a.axisTag='wght';a.minValue=100;a.defaultValue=400;a.maxValue=800 if mode=='half' else 900;a.flags=0;a.axisNameID=256;f['fvar'].axes=[a];f['fvar'].instances=[];f['name'].setName('Weight',256,3,1,0x409)
            f['gvar']=newTable('gvar');f['gvar'].version=1;f['gvar'].reserved=0;f['gvar'].variations={g:[] for g in ORDER}
            f['post'].underlinePosition=-100;f['post'].underlineThickness=50;f['OS/2'].yStrikeoutPosition=300;f['OS/2'].yStrikeoutSize=80
            region=buildVarRegionList([{'wght':(0,1,1)}],['wght'])
            sign=-1 if mode=='negative' else 1
            rows=[[sign*(i*5+1)] for i in range(len(ORDER))]
            if mode in ['fractional','half']:rows=[[sign*(i%7-3)] for i in range(len(ORDER))]
            f['HVAR']=newTable('HVAR');f['HVAR'].table=obj('HVAR',Version=0x10000,VarStore=buildVarStore(region,[buildVarData([0],rows)]),AdvWidthMap=None,LsbMap=None,RsbMap=None)
            if mode=='mapped':f['HVAR'].table.AdvWidthMap=obj('VarIdxMap',mapping={g:i%3 for i,g in enumerate(ORDER)})
            tags=['stro','strs','undo','unds'];deltas=[13,17,19,23]
            if mode=='negative':deltas=[-x for x in deltas]
            if mode in ['fractional','half']:deltas=[-3,-1,1,3]
            values=[obj('MetricsValueRecord',ValueTag=t,VarIdx=i) for i,t in enumerate(tags)]
            f['MVAR']=newTable('MVAR');f['MVAR'].table=obj('MVAR',Version=0x10000,Reserved=0,ValueRecordSize=8,ValueRecordCount=len(values),ValueRecord=values,VarStore=buildVarStore(region,[buildVarData([0],[[d] for d in deltas])]))
            if mode=='empty':f['MVAR'].table.ValueRecord=[];f['MVAR'].table.ValueRecordCount=0;f['MVAR'].table.VarStore=None
            f.save(p)
        with TTFont(p,recalcTimestamp=False) as f:f.saveXML(p.with_suffix('.ttx'))
        files.append(p)
    return files

def run(root,exe):
    files=generate(root/'fixtures');fonts=[]
    for p in files+[Path('C:/Windows/Fonts/bahnschrift.ttf')]:
        cases=[]
        for weight in [400,600]:
            args=[str(exe.resolve()),str(p.resolve()),'61','default','1148846080','0','0','en-US','metrics',str(weight)]
            r=subprocess.run(args,capture_output=True,text=True,timeout=20);assert r.returncode==0,(args,r.stdout,r.stderr);value=json.loads(r.stdout)
            assert value['face_weight_bits']==struct.unpack('<I',struct.pack('<f',weight))[0]
            cases.append(dict(weight=weight,result=value))
        fonts.append(dict(font=identity(p),embedded=p in files,cases=cases))
        print(p.name,[(c['weight'],c['result']['font_metrics'][6:],c['result']['advances'][:8]) for c in cases])
    (root/'observations.json').write_text(json.dumps(dict(reference=identity(exe),fonts=fonts),indent=2)+'\n')

def export(root,header):
    data=json.loads((root/'observations.json').read_text());parts=[];w=lambda *v:struct.pack('<'+'i'*len(v),*v)
    for f in data['fonts']:
        if not f['embedded']:continue
        p=Path(f['font']['path']);assert identity(p)==f['font'];raw=p.read_bytes();part=w(len(raw),len(f['cases']))+raw
        for c in f['cases']:part+=w(c['weight'],len(c['result']['advances']),*c['result']['font_metrics'][6:],*c['result']['advances'])
        parts.append(part)
    raw=b'MCVMET1\0'+w(len(parts))+b''.join(parts);(root/'corpus.bin').write_bytes(raw)
    header.write_text('// First-party variable metric fonts and Windows design-unit expectations.\nconstexpr u8 metric_variations_fixture[]{\n'+'\n'.join('    '+','.join(f'0x{x:02x}' for x in raw[i:i+24])+',' for i in range(0,len(raw),24))+'\n};\n')
    print('exported',len(parts),'fonts',len(raw),'bytes')
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('root',type=Path);p.add_argument('--exe',type=Path);p.add_argument('--header',type=Path);a=p.parse_args();a.root.mkdir(parents=True,exist_ok=True)
    if a.exe:run(a.root,a.exe)
    if a.header:export(a.root,a.header)
