"""First-party Latin class-refresh, root-filter and variable-anchor probes.

FontTools creates fixtures only; installed Windows supplies all expected glyphs,
clusters and placement bits. Use latin_run_export.py for native-only comparison.
"""
from pathlib import Path
import argparse, copy, json, subprocess
from fontTools.ttLib import TTFont,newTable
from fontTools.ttLib.tables._f_v_a_r import Axis
from fontTools.varLib.builder import buildVarRegionList,buildVarData,buildVarStore
from gsub_execution_oracle import make_font,lookup,single,multiple,context,obj,ORDER
from gpos_execution_oracle import mark_base
from latin_preprocess_oracle import identity

def features(table, rows):
    table.FeatureList.FeatureRecord=[obj('FeatureRecord',FeatureTag=tag,Feature=obj('Feature',FeatureParams=None,LookupCount=len(roots),LookupListIndex=roots)) for tag,roots in rows]
    table.FeatureList.FeatureCount=len(rows)
    for r in table.ScriptList.ScriptRecord:
        lang=r.Script.DefaultLangSys;lang.FeatureCount=len(rows);lang.FeatureIndex=list(range(len(rows)))

def generate(root):
    root.mkdir(parents=True,exist_ok=True);cases=[]
    def base(name,lookups):
        path=root/(name+'.ttf');make_font(path,dict(name=name,lookups=lookups));return path
    roots=[multiple({'A':['A','M']}),context('AB',[(1,2)],flags=8),lookup(1,single({'B':'X'}))]
    for name,rows in [('ccmp-roots',[('ccmp',[0,1])]),('ccmp-calt',[('calt',[1]),('ccmp',[0])]),('calt-roots',[('calt',[0,1])])]:
        path=base(name,copy.deepcopy(roots))
        with TTFont(path,recalcTimestamp=False) as f:features(f['GSUB'].table,rows);f.save(path)
        cases.append((path,'ab',0))
    nested=[context('AB',[(0,1),(0,2)]),multiple({'A':['A','M']}),context('AB',[(1,3)],flags=8),lookup(1,single({'B':'X'}))]
    root_single=[lookup(1,single({'A':'M'})),context('MB',[(1,2)],flags=8),lookup(1,single({'B':'X'}))]
    for name,lookups,scheduled in [('nested-class',nested,[0]),('root-first-filter',root_single,[0,1])]:
        path=base(name,lookups)
        with TTFont(path,recalcTimestamp=False) as f:features(f['GSUB'].table,[('ccmp',scheduled)]);f.save(path)
        cases.append((path,'ab',0))
    for count in range(1,6):
        path=base(f'null-{count}',[lookup(1,single({'A':'A'}))])
        with TTFont(path,recalcTimestamp=False) as f:
            table=copy.deepcopy(f['GSUB'].table);del f['GSUB'];table.LookupList.Lookup=[mark_base(bases='ABCDE'[:count],null=True)];table.LookupList.LookupCount=1
            features(table,[('mark',[0])]);f['GPOS']=newTable('GPOS');table.__class__=type(obj('GPOS'));f['GPOS'].table=table;f.save(path)
        cases.append((path,'a\u0327',0))
    for delta in (-3,-1,1,3):
        path=base(f'anchor-{delta}',[lookup(1,single({'A':'A'}))])
        with TTFont(path,recalcTimestamp=False) as f:
            table=copy.deepcopy(f['GSUB'].table);del f['GSUB'];look=mark_base();a=look.SubTable[0].BaseArray.BaseRecord[0].BaseAnchor[0]
            a.Format=3;a.XCoordinate=0;a.YCoordinate=0;a.XDeviceTable=obj('Device',StartSize=0,EndSize=0,DeltaFormat=0x8000);a.YDeviceTable=None
            table.LookupList.Lookup=[look];table.LookupList.LookupCount=1;features(table,[('mark',[0])])
            f['GPOS']=newTable('GPOS');table.__class__=type(obj('GPOS'));f['GPOS'].table=table
            f['fvar']=newTable('fvar');axis=Axis();axis.axisTag='wght';axis.minValue=100;axis.defaultValue=400;axis.maxValue=800;axis.flags=0;axis.axisNameID=256
            f['fvar'].axes=[axis];f['fvar'].instances=[];f['name'].setName('Weight',256,3,1,0x409)
            f['gvar']=newTable('gvar');f['gvar'].version=1;f['gvar'].reserved=0;f['gvar'].variations={g:[] for g in ORDER}
            region=buildVarRegionList([{'wght':(0,1,1)}],['wght'])
            f['HVAR']=newTable('HVAR');f['HVAR'].table=obj('HVAR',Version=0x10000,VarStore=buildVarStore(region,[buildVarData([0],[[0] for g in ORDER])]),AdvWidthMap=None,LsbMap=None,RsbMap=None)
            gdef=f['GDEF'].table;gdef.Version=0x10003;gdef.MarkGlyphSetsDef=None;gdef.VarStore=buildVarStore(region,[buildVarData([0],[[delta]])]);f.save(path)
        cases.append((path,'a\u0301',600))
    return cases

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('root',type=Path);p.add_argument('--exe',required=True,type=Path);a=p.parse_args();rows=[]
    for path,text,weight in generate(a.root/'fonts'):
        for bits in (1098907648,1099694080,1148846080):
            command=[str(a.exe),str(path),','.join(f'{ord(c):x}' for c in text),'default',str(bits),'0','0','en-US']
            if weight:command+=['layout',str(weight)]
            run=subprocess.run(command,capture_output=True,text=True,check=True);r=json.loads(run.stdout)
            for key in list(r):
                if key.startswith('layout_'):del r[key]
            rows.append(dict(font=identity(path),text=[ord(c) for c in text],weight=weight,exit=run.returncode,result=r))
    data=dict(executable=identity(a.exe),dwrite=identity(Path(rows[0]['result']['dwrite_module'])),rows=rows)
    (a.root/'latin-compat-oracle.json').write_text(json.dumps(data,indent=2));print(len(rows),'Latin compatibility cases')
if __name__=='__main__':main()
