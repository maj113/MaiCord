"""Authored Tibetan cross-cluster fixtures and complete scalar runs; QA only."""
import argparse,json,itertools
from pathlib import Path
from fontTools.ttLib import TTFont,newTable
from gsub_execution_oracle import make_font,lookup,single,ligature,multiple,context
from feature_plan_oracle import layout,script,language
from gpos_execution_oracle import pair,mark_lig,adjust
from fontTools.ttLib.tables._c_m_a_p import CmapSubtable
from tibetan_input_samples import make_font as identity_font


def generate(folder):
    folder.mkdir(parents=True,exist_ok=True)
    cmap={0x20:'tagC',0xf40:'A',0xf41:'B',0xf42:'C',0xf90:'D',0xf3f:'E',
          0xf71:'X',0xf72:'Y',0xf80:'Z',0xf74:'M',0xfb2:'N',0x200d:'tagA',
          0x200c:'tagB',0x34f:'tagD',0x25cc:'L'}
    configs={
      'pair':([],[],[pair(15)],'dist'),
      'early-lig':([ligature('AB','L')],[('ccmp',[0])],[],'dist'),
      'isol-lig':([ligature('AB','L')],[('isol',[0])],[],'dist'),
      'lig-mark':([ligature('AB','L',8)],[('pres',[0])],[mark_lig()],'mark'),
      'lig-split':([ligature('AB','L',8),multiple({'L':['X','Y']})],[('pres',[0,1])],[mark_lig()],'mark'),
      'lig-expanded':([ligature('AB','L',8),multiple({'L':['X','Y','Z']})],[('pres',[0,1])],[],'dist'),
      'early-expanded':([multiple({'A':['A','X']})],[('ccmp',[0])],[],'dist'),
      'early-context':([context('AB',[(1,1)]),lookup(1,single({'B':'X'}))],[('ccmp',[0])],[],'dist'),
      'late-context':([context('AB',[(1,1)]),lookup(1,single({'B':'X'}))],[('pres',[0])],[],'dist'),
      'late-controls':([ligature('AB','L',8)],[('pres',[0])],[adjust('tagC',13,17,31),adjust('tagD',19,23,37)],'dist'),
      'early-late':([ligature(['B','D','tagA'],'Y'),ligature(['D','tagA','C'],'Z'),ligature('AB','L',8)], [('pref',[1]),('pres',[2]),('rphf',[0])],[mark_lig()],'mark'),
    }
    for name,(subs,features,pos,tag) in configs.items():
        path=folder/(name+'.ttf');make_font(path,dict(name='tibetan-run-'+name,lookups=[lookup(1,single({'A':'A'}))]))
        with TTFont(path,recalcTimestamp=False) as f:
            for c in f['cmap'].tables:
                if c.isUnicode():c.cmap=cmap.copy()
            f['GDEF'].table.GlyphClassDef.classDefs.update(D=3,tagD=3)
            f['GSUB'].table=layout('GSUB',features,subs,{'tibt':script(language(range(len(features))))})
            if pos:
                f['GPOS']=newTable('GPOS');f['GPOS'].table=layout('GPOS',[(tag,list(range(len(pos))))],pos,{'tibt':script(language([0]))})
            f.save(path)
    parts=[[0xf40],[0xf41],[0xf40,0xf3f],[0xf41,0xf3f],[0xf40,0xf73],
      [0xf41,0xf74],[0xf40,0x34f],[0xf40,0x200d],[0x20],[0xf40,0xf90],
      [0xf41,0xf90,0x200d,0xf40,0xf3f],[0xf40,0xf90,0x200d,0xf42,0xf3f]]
    cases={tuple(a+b) for a in parts for b in parts}
    cases.update(tuple([0xf40,0xf41]*n) for n in (1,2,8,16,32,64,128))
    cases.update(tuple([0xf40]+[0xf90]*n+[0xf40,0xf73]) for n in (29,30,31,32,61,62,63,125))
    cases.update(tuple(s) for s in [[0xf40,0xf74,0xf41,0xf74],[0xf40,0xf41,0xf74],
      [0xf3f,0xf40,0xf41],[0xf40,0xf3f,0xf41,0xf74],[0xf40,0xf41,0xf3f,0x34f]])
    (folder/'cases.json').write_text(json.dumps(sorted(cases)),encoding='utf-8')
    generate_selectors(folder)
    # Every pair of representative cluster/control/selector fragments, plus
    # maximal clusters crossing the 63-scalar mapping window.
    parts += [[c] for c in (0xf73,0xf76,0xf78,0xf81,0xf7f,0xf90,0xf3e,0x25cc,0xfe00,0xe0100,0xe01ef,0x200c,0x200d,0x200e,0x200f,0x2060,0x34f)]
    parts += [[c,s] for c in (0xf40,0xf71,0xf73,0xf76,0xf78,0xf81) for s in (0xfe00,0xfe0f,0xe0100,0xe01ef)]
    broad={tuple(a+b) for a,b in itertools.product(parts,repeat=2)}
    for n in (29,30,31,32,60,61,62):
        for selector in (0xfe00,0xe0100,0xe01ef):
            broad.add(tuple([0xf40,selector]+[v for _ in range(n) for v in (0xf90,selector)]+[0xf40,0xf73]))
    broad.update(cases)
    (folder/'broad-cases.json').write_text(json.dumps(sorted(broad)),encoding='utf-8')


def generate_selectors(folder):
    path=folder/'selectors-full.ttf';identity_font(path)
    with TTFont(path,recalcTimestamp=False) as f:
        table=CmapSubtable.newSubtable(14);table.platformID=0;table.platEncID=5;table.language=0;table.cmap={}
        selectors=list(range(0xfe00,0xfe10))+list(range(0xe0100,0xe01f0))
        table.uvsDict={s:[(c,f'u{0xf10+(i%48):04x}') for c in (0xf40,0xf71,0xf72,0xf80,0xfb2,0xfb3)] for i,s in enumerate(selectors)}
        f['cmap'].tables.append(table);f.save(path)
    history=set()
    for selector in selectors:
        for vowel in (0xf73,0xf76,0xf78,0xf81):
            history.add((0xf40,0xf71,selector,0xf72,selector,0xf40,vowel))
            history.add((0xf40,0xf71,selector,vowel))
    history.add(tuple([0xf40,0xfe00]+[v for _ in range(30) for v in (0xf90,0xe01ef)]+[0xf40,0xf73]))
    (folder/'selector-cases.json').write_text(json.dumps(sorted(history)),encoding='utf-8')


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('folder',type=Path)
    generate(p.parse_args().folder)
