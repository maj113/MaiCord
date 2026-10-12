"""Author control-ID and variation-selector Tibetan shaping fixtures; QA only."""
import json
from pathlib import Path
from fontTools.ttLib import TTFont,newTable
from fontTools.ttLib.tables._c_m_a_p import CmapSubtable
from tibetan_input_samples import make_font
from feature_plan_oracle import layout,script,language
from gsub_execution_oracle import lookup,single,ligature
from gpos_execution_oracle import adjust

def build(output,selected=None):
    fonts=[]
    for name in ('control-defined','control-alias','substitute-control','ligature-control','substitute-away','selectors','control-position'):
        if selected and name not in selected:continue
        path=output/(name+'.ttf');make_font(path)
        with TTFont(path,recalcTimestamp=False) as font:
            for table in font['cmap'].tables:
                if not table.isUnicode():continue
                if name=='control-defined':table.cmap.update({0x200c+i:f'u{0xf10+i:04x}' for i in range(4)})
                if name=='control-alias':table.cmap.update({0x200c:'u0f40',0x200d:'u0f40',0xf90:'u0f40'})
            changes=None
            if name=='substitute-control':changes=lookup(1,single({'u0f40':'u200c','u0f90':'u0f41'}))
            if name=='ligature-control':changes=ligature(['u0f40','u0f90'],'u200d')
            if name=='substitute-away':changes=lookup(1,single({'u200d':'u0f42'}))
            if changes:
                font['GSUB']=newTable('GSUB');font['GSUB'].table=layout('GSUB',[('ccmp',[0])],[changes],{'tibt':script(language([0]))})
            if name=='selectors':
                table=CmapSubtable.newSubtable(14);table.platformID=0;table.platEncID=5;table.language=0;table.cmap={}
                table.uvsDict={0xfe00:[(0xf40,'u0f41'),(0xf71,'u0f42'),(0xf72,'u0f43'),(0xf73,'u0f44'),(0xf76,'u0f45'),(0xf80,'u0f46'),(0xfb2,'u0f47')],
                    0xe0100:[(0xf40,'u0f49'),(0xf71,'u0f4a'),(0xf72,'u0f4b'),(0xf73,'u0f4c'),(0xf76,'u0f4d'),(0xf80,'u0f4e'),(0xfb2,'u0f4f')]}
                font['cmap'].tables.append(table)
            if name=='control-position':
                font['GPOS']=newTable('GPOS');font['GPOS'].table=layout('GPOS',[('dist',[0])],[adjust('u0020',11,17,31)],{'tibt':script(language([0]))})
            font.save(path)
        fonts.append(path)
    return fonts

def cases(output):
    values=set()
    alphabet={0xf40,0xf90,0xf71,0xf72,0xf73,0xf76,0xf77,0xf78,0xf79,0xf81,0xf7f,0x34f,0x200c,0x200d,0xfe00,0xfe01,0xe0100,0x25fc,0x301}
    for path in output.glob('cases-*.json'):
        for s in json.loads(path.read_text()):
            if len(s)<=4 and all(c in alphabet for c in s):values.add(tuple(s))
    for c in (0xf40,0xf73,0xf76,0xf78,0xf81):
        for selector in (0xfe00,0xe0100):values.add((c,selector));values.add((0xf40,c,selector)) if c!=0xf40 else None
    for c in (0x200e,0x200f):values.add((c,))
    path=output/'special-cases.json';path.write_text(json.dumps(sorted(values)));return path
