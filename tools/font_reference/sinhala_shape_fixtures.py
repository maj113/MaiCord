"""Small first-party USE stage/reordering fixtures; offline FontTools only."""
import argparse
import json
from pathlib import Path
from fontTools.ttLib import TTFont,newTable
from gsub_execution_oracle import make_font,lookup,single,ligature,multiple
from feature_plan_oracle import layout,script,language
from gpos_execution_oracle import adjust


def generate(folder):
    folder.mkdir(parents=True,exist_ok=True)
    cmap={0x20:'tagC',0xd9a:'A',0xdbb:'B',0xdba:'C',0xdca:'D',0xdd9:'E',0xdcf:'X',
          0xddd:'Y',0xddf:'Z',0x200d:'tagA',0x200c:'tagB',0x34f:'tagD',0x25cc:'L'}
    for name in ('forms','dist','expanded-pre','expanded-pre-three','pref-last','pref-expand','dist-controls'):
        path=folder/(name+'.ttf')
        make_font(path,dict(name='sinhala-'+name,lookups=[lookup(1,single({'A':'A'}))]))
        with TTFont(path,recalcTimestamp=False) as font:
            for sub in font['cmap'].tables:
                if sub.isUnicode():sub.cmap=cmap.copy()
            font['GDEF'].table.GlyphClassDef.classDefs.update(D=3,tagD=3)
            roots=[ligature(['B','D','tagA'],'M'),ligature(['D','tagA','C'],'N')]
            features=[('pref',[1]),('rphf',[0])]
            if name=='pref-last':
                roots.append(ligature(['D','tagA','B'],'X'));features[0]=('pref',[1,2])
            if name.startswith('expanded-pre'):
                roots.append(multiple({'E':['E','X','Y'] if name.endswith('three') else ['E','X']}));features.insert(0,('ccmp',[2]))
            if name=='pref-expand':
                roots=[multiple({'B':['B','X']})];features=[('pref',[0])]
            font['GSUB'].table=layout('GSUB',features,roots,{'sinh':script(language(range(len(features))))})
            if name in ('dist','dist-controls'):
                font['GPOS']=newTable('GPOS')
                positions=[adjust('M',0,0,125)] if name=='dist' else [adjust('tagC',13,17,31),adjust('tagD',19,23,37)]
                font['GPOS'].table=layout('GPOS',[('dist',list(range(len(positions))))],positions,{'sinh':script(language([0]))})
            font.save(path)
    cases=[[0xdbb,0xdca,0x200d,0xd9a,0xdd9],
           [0xd9a,0xdca,0x200d,0xdba,0xdd9],
           [0xdbb,0xdca,0x200d,0xd9a,0xdca,0x200d,0xdba,0xdd9],
           [0xd9a,0xdca,0xdbb,0xdca,0x200d,0xd9a,0xdd9],
           [0xdbb,0xdca,0x200d,0xd9a,0xdca,0xd9a,0xdd9],
           [0xd9a,0xdca,0xd9a,0xdd9],[0xdd9],[0xd9a,0xdd9,0x34f],
           [0xd9a,0xdd9,0x200d,0xdd9]]
    (folder/'cases.json').write_text(json.dumps(cases),encoding='utf-8')
    deep=[]
    for n in range(8):
        deep.append([0xd9a,0xdca]*n+[0xdbb,0xdca,0x200d,0xd9a,0xdd9])
        if n:deep.append([0xdbb,0xdca,0x200d]*n+[0xd9a,0xdd9])
    tails=[[0xdd9]+[0x34f]*n for n in range(7)]
    tails.extend([[0xdd9,0x200d,0xdd9],[0xdd9,0x34f,0x200d,0xdd9],
                  [0xdd9,0xdd9,0x34f],[0xdd9,0x200d,0x34f,0xdd9],
                  [0xdd9,0x200d,0xdd9,0x34f]])
    for prefix in ([0xd9a],[0xdbb,0xdca,0x200d,0xd9a],[0xd9a,0xdca,0x200d,0xdba],[0xd9a,0xdca,0xd9a]):
        deep.extend(prefix+tail for tail in tails)
    (folder/'deep-cases.json').write_text(json.dumps(deep),encoding='utf-8')
    pref=[]
    for one,two in ((0xdba,0xdbb),(0xdbb,0xdba)):
        for tail in ([],[0xdd9],[0xdd9,0x34f]):
            pref.append([0xd9a,0xdca,0x200d,one,0xdca,0x200d,two]+tail)
    (folder/'pref-cases.json').write_text(json.dumps(pref),encoding='utf-8')
    (folder/'pref-expansion-cases.json').write_text(json.dumps([[0xd9a,0xdca,0xdbb],
        [0xd9a,0xdca,0xdbb,0xdd9],[0xd9a,0xdca,0xdbb,0xdd9,0x34f],[0xdbb]]),encoding='utf-8')
    (folder/'position-cases.json').write_text(json.dumps([[0xd9a,0x200d],[0xd9a,0x34f],
        [0xd9a,0xdd9,0x34f],[0x20],[0xdbb,0xdca,0x200d,0xd9a,0xdd9]]),encoding='utf-8')


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('folder',type=Path)
    generate(p.parse_args().folder)
