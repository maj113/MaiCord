"""Authored Sinhala run contexts/ligatures/GPOS fixtures; offline QA only."""
import argparse,json
from pathlib import Path
from fontTools.ttLib import TTFont,newTable
from gsub_execution_oracle import make_font,lookup,single,ligature,multiple,context
from feature_plan_oracle import layout,script,language
from gpos_execution_oracle import pair,mark_lig,adjust


def generate(folder):
    folder.mkdir(parents=True,exist_ok=True)
    cmap={0x20:'tagC',0xd9a:'A',0xdbb:'B',0xdba:'C',0xdca:'D',0xdd9:'E',
          0xdcf:'X',0xddd:'Y',0xddf:'Z',0xdd2:'M',0xdd4:'N',0x200d:'tagA',
          0x200c:'tagB',0x34f:'tagD',0x25cc:'L'}
    configs={
      'pair':([],[],[pair(15)],'dist'),
      'lig-mark':([ligature('AB','L',8)],[('pres',[0])],[mark_lig()],'mark'),
      'lig-split':([ligature('AB','L',8),multiple({'L':['X','Y']})],[('pres',[0,1])],[mark_lig()],'mark'),
      'lig-expanded':([ligature('AB','L',8),multiple({'L':['X','Y','Z']})],[('pres',[0,1])],[],'dist'),
      'early-expanded':([multiple({'A':['A','X']})],[('ccmp',[0])],[],'dist'),
      'late-context':([context('AB',[(1,1)]),lookup(1,single({'B':'X'}))],[('pres',[0])],[],'dist'),
      'late-controls':([ligature('AB','L',8)],[('pres',[0])],[adjust('tagC',13,17,31),adjust('tagD',19,23,37)],'dist'),
      'early-late':([ligature(['B','D','tagA'],'Y'),ligature(['D','tagA','C'],'Z'),ligature('AB','L',8)], [('pref',[1]),('pres',[2]),('rphf',[0])],[mark_lig()],'mark'),
    }
    for name,(subs,features,pos,tag) in configs.items():
        path=folder/(name+'.ttf');make_font(path,dict(name='sinhala-run-'+name,lookups=[lookup(1,single({'A':'A'}))]))
        with TTFont(path,recalcTimestamp=False) as f:
            for c in f['cmap'].tables:
                if c.isUnicode():c.cmap=cmap.copy()
            f['GDEF'].table.GlyphClassDef.classDefs.update(D=3,tagD=3)
            f['GSUB'].table=layout('GSUB',features,subs,{'sinh':script(language(range(len(features))))})
            if pos:
                f['GPOS']=newTable('GPOS');f['GPOS'].table=layout('GPOS',[(tag,list(range(len(pos))))],pos,{'sinh':script(language([0]))})
            f.save(path)
    parts=[[0xd9a],[0xdbb],[0xd9a,0xdd9],[0xdbb,0xdd9],[0xd9a,0xdd2],
      [0xdbb,0xdd4],[0xd9a,0x34f],[0xd9a,0x200d],[0x20],
      [0xdbb,0xdca,0x200d,0xd9a,0xdd9],[0xd9a,0xdca,0x200d,0xdba,0xdd9]]
    cases=[a+b for a in parts for b in parts]
    cases += [[0xd9a,0xdbb]*n for n in (1,2,8,16,32,64)]
    cases += [[0xd9a,0xdd2,0xdbb,0xdd4],[0xd9a,0xdbb,0xdd2],[0xdd9,0xd9a,0xdbb],
              [0xd9a,0xdd9,0xdbb,0xdd2],[0xd9a,0xdbb,0xdd9,0x34f]]
    (folder/'cases.json').write_text(json.dumps(cases),encoding='utf-8')


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('folder',type=Path)
    generate(p.parse_args().folder)
