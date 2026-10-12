"""Offline Myanmar run-scope and cross-cluster substitution/position probes."""
import argparse,json
from pathlib import Path
from fontTools.ttLib import TTFont
from myanmar_input_font import make_font
from feature_plan_oracle import layout,script,language
from gsub_execution_oracle import ligature,multiple,lookup,single,context
from gpos_execution_oracle import lookup as pos_lookup,coverage,obj,value

FEATURES=('locl','ccmp','nukt','akhn','rphf','pref','rkrf','abvf','blwf','half','pstf','vatu','cjct',
          'isol','abvs','blws','calt','clig','haln','liga','pres','psts','rclt','rlig','rvrn')
def generate(folder):
    folder.mkdir(parents=True,exist_ok=False);base=folder/'identity.ttf';make_font(base)
    for feature in FEATURES:
        with TTFont(base,recalcTimestamp=False) as f:
            f['GSUB'].table=layout('GSUB',[(feature,[0])],[ligature(['u1000','u1000'],'u1001')],{'mym2':script(language([0]))})
            f.save(folder/(feature+'.ttf'))
    with TTFont(base,recalcTimestamp=False) as f:
        for name,features,lookups in (
            ('decompose',[('pres',[0,1])],[ligature(['u1000','u1000'],'u1001'),multiple({'u1001':['u1000','u1000']})]),
            ('early-context',[('ccmp',[0])],[context(['u1000','u1000'],[(1,1)]),lookup(1,single({'u1000':'u1001'}))]),
            ('late-context',[('pres',[0])],[context(['u1000','u1000'],[(1,1)]),lookup(1,single({'u1000':'u1001'}))])):
            f['GSUB'].table=layout('GSUB',features,lookups,{'mym2':script(language(list(range(len(features)))))})
            f.save(folder/(name+'.ttf'))
    with TTFont(base,recalcTimestamp=False) as f:
        records=[obj('PairValueRecord',SecondGlyph='u1000',Value1=value(11,17,31),Value2=value(-23,29,41))]
        pair=pos_lookup(2,obj('PairPos',Format=1,Coverage=coverage(['u1000']),ValueFormat1=15,ValueFormat2=15,
            PairSetCount=1,PairSet=[obj('PairSet',PairValueCount=1,PairValueRecord=records)]))
        f['GPOS'].table=layout('GPOS',[('dist',[0])],[pair],{'mym2':script(language([0]))})
        f.save(folder/'pair.ttf')
    cases=[[0x1000,0x1000],[0x1000]*3,[0x1000,0x1031,0x1000],
           [0x1000,0x1031,0x1000,0x1031],[0x1000,0x1039,0x1000,0x1000],
           [0x1000,0x1039,0x1000,0x1031,0x1000,0x1031],[0x1000,0xfe00,0x1000],
           [0x1000,0xe0100,0x1000],[0x1000,0x200d,0x1000],[0x1004,0x103a,0x1039,0x104a,0x1000],
           [0x1004,0x103a,0x1039,0xfe00,0x104a],[0x1004,0x103a,0x1039,0x1000,0x1031]*2]
    (folder/'cases.json').write_text(json.dumps(cases))

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('folder',type=Path);generate(p.parse_args().folder)
