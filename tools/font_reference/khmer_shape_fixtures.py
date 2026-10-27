"""First-party Khmer reordering/feature probes; offline font authoring only."""
import argparse,json
from pathlib import Path
from fontTools.ttLib import TTFont,newTable
from fontTools.ttLib.tables._g_l_y_f import Glyph
from khmer_expansion_oracle import make_font
from gsub_execution_oracle import lookup,single
from feature_plan_oracle import layout,script,language

def generate(folder):
    folder.mkdir(parents=True,exist_ok=True);base=folder/'identity.ttf';make_font(base)
    with TTFont(base,recalcTimestamp=False) as f:
        extra=(0x20,0xa0,0x34f,0x200c,0x200d,0xfe00,0xfe01)
        order=f.getGlyphOrder()+[f'u{c:04x}' for c in extra];f.setGlyphOrder(order)
        for c in extra:
            g=f'u{c:04x}';f['glyf'].glyphs[g]=Glyph();f['hmtx'].metrics[g]=(600,0)
            for cmap in f['cmap'].tables:
                if cmap.isUnicode():cmap.cmap[c]=g
        f.save(base)
    with TTFont(base,recalcTimestamp=False) as f:
        original=f.getGlyphOrder()[1:];f.setGlyphOrder(f.getGlyphOrder()+['tag'+g for g in original])
        for g in original:f['glyf'].glyphs['tag'+g]=Glyph();f['hmtx'].metrics['tag'+g]=(600,0)
        f['GSUB'].table=layout('GSUB',[('cfar',[0])],[lookup(1,single({g:'tag'+g for g in original}))],{'khmr':script(language([0]))})
        f.save(folder/'cfar.ttf')
    cases=[]
    for head in ([0x1780,0x17d2,0x179a],[0x1780,0x17d2,0x1780,0x17d2,0x179a],
                 [0x1780,0x17c9,0x17d2,0x179a],[0x17d2,0x179a]):
        for tail in ([],[0x17c1],[0x17be],[0x17b7],[0x17c6],[0x17c9],[0x200c],[0x200d],[0x34f],[0xfe00],
                     [0x17d2],[0x17d2,0x1780],[0x17d2,0x1799],[0x17d2,0x179a],[0x17d2,0x17a5]):cases.append(head+tail)
    for vowel in (0x17c1,0x17c2,0x17c3):
        for middle in ([0x17c6],[0x17d2,0x179a],[0x17d2,0x1780]):cases.append([0x1780,vowel]+middle+[0x17c4])
    (folder/'feature-cases.json').write_text(json.dumps(cases))
def write_unit_fixtures(destination):
    from gsub_execution_oracle import obj,ligature,multiple
    from gpos_execution_oracle import adjust
    font=TTFont();font.setGlyphOrder(['g'+str(i) for i in range(32)])
    font['GSUB']=newTable('GSUB');font['GSUB'].table=layout('GSUB',[('ccmp',[0,1]),('pres',[2])],
        [lookup(1,single({'g2':'g7','g7':'g2'})),ligature(['g2','g2'],'g3'),multiple({'g3':['g2','g2']})],{'khmr':script(language([0,1]))})
    font['GPOS']=newTable('GPOS');font['GPOS'].table=layout('GPOS',[('dist',[0,1])],
        [adjust('g4',11,17,31),adjust('g1',13,19,37)],{'khmr':script(language([0]))})
    font['GDEF']=newTable('GDEF');font['GDEF'].table=obj('GDEF',Version=0x10000,AttachList=None,
        LigCaretList=None,MarkAttachClassDef=None,GlyphClassDef=obj('ClassDef',classDefs={'g4':3}))
    lines=['// First-party data from khmer_shape_fixtures.write_unit_fixtures.']
    for tag in ('GSUB','GPOS','GDEF'):
        data=font[tag].compile(font);lines.append('constexpr u8 khmer_shape_'+tag.lower()+'[]{')
        lines.extend('    '+','.join(str(v) for v in data[i:i+32])+',' for i in range(0,len(data),32));lines.append('};')
    destination.write_text('\n'.join(lines)+'\n')

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('folder',type=Path);generate(p.parse_args().folder)
