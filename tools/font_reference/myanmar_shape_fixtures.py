"""First-party Myanmar control, selector, staged GSUB and positioning probes.

fontTools authors offline QA fonts only; Windows supplies expected output.
"""
import argparse,json
from pathlib import Path
from fontTools.ttLib import TTFont
from fontTools.ttLib.tables._c_m_a_p import CmapSubtable
from fontTools.ttLib.tables._g_l_y_f import Glyph
from myanmar_input_font import make_font
from feature_plan_oracle import layout,script,language
from gsub_execution_oracle import lookup,single,ligature,multiple,obj
from gpos_execution_oracle import adjust

def generate(folder):
    folder.mkdir(parents=True,exist_ok=False)
    base=folder/'identity.ttf';make_font(base)
    with TTFont(base,recalcTimestamp=False) as font:
        extra=[0x200b,0x200e,0x200f,0xfeff]
        order=font.getGlyphOrder()+[f'u{c:04x}' for c in extra]+['variantC','variantE','variantR','variantCircle','variantNGA','variantH']
        font.setGlyphOrder(order)
        for g in order:
            if g not in font['glyf'].glyphs:
                font['glyf'].glyphs[g]=Glyph();font['hmtx'].metrics[g]=(600,0)
        for cmap in font['cmap'].tables:
            if cmap.isUnicode():cmap.cmap.update({c:f'u{c:04x}' for c in extra})
        font.save(base)
    with TTFont(base,recalcTimestamp=False) as f:
        table=CmapSubtable.newSubtable(14);table.platformID=0;table.platEncID=5;table.language=0;table.cmap={}
        table.uvsDict={c:[(0x1000,'variantC'),(0x1004,'variantNGA'),(0x1031,'variantE'),
                         (0x1039,'variantH'),(0x103c,'variantR'),(0x25cc,'variantCircle')] for c in (0xfe00,0xe0100)}
        f['cmap'].tables.append(table);f.save(folder/'selectors.ttf')
    with TTFont(base,recalcTimestamp=False) as f:
        f['GSUB'].table=layout('GSUB',[('ccmp',[0])],
            [lookup(1,single({'u1000':'u200d','u200d':'u1000','u1001':'.notdef','.notdef':'u1001'}))],{'mym2':script(language([0]))})
        f.save(folder/'controls.ttf')
    with TTFont(base,recalcTimestamp=False) as f:
        f['GDEF'].table.GlyphClassDef=obj('ClassDef',classDefs={'u103c':3,'u102d':3})
        f['GPOS'].table=layout('GPOS',[('dist',[0,1,2])],
            [adjust('u103c',11,17,31),adjust('u0020',13,19,37),adjust('u102d',-23,29,41)],{'mym2':script(language([0]))})
        f.save(folder/'positions.ttf')
    with TTFont(base,recalcTimestamp=False) as f:
        features=[('ccmp',[0]),('rphf',[1]),('pref',[2]),('blwf',[3]),('isol',[4]),('pres',[5])]
        f['GSUB'].table=layout('GSUB',features,[lookup(1,single({'u1000':'u1001'})),
            ligature(['u1004','u103a','u1039'],'u1002'),lookup(1,single({'u103c':'u1003'})),
            ligature(['u1039','u1001'],'u1005'),lookup(1,single({'u1003':'u1006'})),multiple({'u1002':['u1007','u1008']})],
            {'mym2':script(language(list(range(len(features)))))})
        f.save(folder/'stages.ttf')
    cases={(c,) for c in (0,0x1000,0x1001,0x103c,0x1031,0x102d,0x34f,0x200b,0x200c,0x200d,0x200e,0x200f,0xfeff,0x2060,0xfe00,0xfe01,0xe0100,0x1f600,0x10ffff)}
    for s in ([0x1000],[0x103c],[0x1031],[0x1039],[0x1000,0x103c],[0x1000,0x1031],
              [0x1004,0x103a,0x1039],[0x1004,0x103a,0x1039,0x1000,0x103c,0x1031],
              [0x1000,0x1039,0x1000],[0x1000,0x103c,0x102d],[0x102d]):
        cases.add(tuple(s))
        for selector in (0xfe00,0xfe01,0xe0100):
            for at in range(1,len(s)+1):cases.add(tuple(s[:at]+[selector]+s[at:]))
        if s[0] not in (0x103c,0x1031,0x102d,0x1039):
            for joiner in (0x200c,0x200d):cases.add(tuple(s+[joiner]))
    (folder/'cases.json').write_text(json.dumps(sorted(cases)))

def write_units(destination):
    from fontTools.ttLib import newTable
    f=TTFont();f.setGlyphOrder(['g'+str(i) for i in range(32)])
    f['GSUB']=newTable('GSUB');f['GSUB'].table=layout('GSUB',[('ccmp',[0]),('rphf',[1]),('pres',[2])],
        [lookup(1,single({'g2':'g7','g7':'g2'})),ligature(['g3','g2'],'g4'),multiple({'g4':['g2','g3']}),
         ligature(['g2','g2'],'g4'),multiple({'g4':['g4','g4','g4']})],
        {'mym2':script(language([0,1,2]))})
    f['GPOS']=newTable('GPOS');f['GPOS'].table=layout('GPOS',[('dist',[0,1])],
        [adjust('g4',11,17,31),adjust('g1',13,19,37)],{'mym2':script(language([0]))})
    f['GDEF']=newTable('GDEF');f['GDEF'].table=obj('GDEF',Version=0x10000,AttachList=None,LigCaretList=None,
        MarkAttachClassDef=None,GlyphClassDef=obj('ClassDef',classDefs={'g4':3}))
    variants=CmapSubtable.newSubtable(14);variants.platformID=0;variants.platEncID=5;variants.language=0;variants.cmap={}
    variants.uvsDict={c:[(0x1000,'g9'),(0x1031,'g10'),(0x103c,'g11'),(0x25cc,'g12')] for c in (0xfe00,0xe0100,0xe01ef)}
    tables=[(tag.lower(),f[tag].compile(f)) for tag in ('GSUB','GPOS','GDEF')]+[('variants',variants.compile(f))]
    lines=['// First-party data from myanmar_shape_fixtures.write_units.']
    for name,data in tables:
        lines.append('constexpr u8 myanmar_shape_'+name+'[]{')
        lines.extend('    '+','.join(str(v) for v in data[i:i+32])+',' for i in range(0,len(data),32));lines.append('};')
    destination.write_text('\n'.join(lines)+'\n')

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('folder',type=Path)
    generate(parser.parse_args().folder)
