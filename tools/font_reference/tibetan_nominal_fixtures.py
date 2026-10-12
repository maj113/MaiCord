"""Authored Tibetan non-admitted control/selector fixtures; offline QA only."""
import argparse,json
from pathlib import Path
from fontTools.ttLib import TTFont,newTable
from tibetan_shape_fixtures import build
from tibetan_input_samples import make_font
from tibetan_admission_policy import CASES
from feature_plan_oracle import layout,script,language
from gsub_execution_oracle import ligature,multiple

def extra_fonts(folder):
    configs={'lig-from-control':[ligature(['u200d','u0f40'],'u0f41'),ligature(['u0f40','u200d'],'u0f42')],
             'expand-control':[multiple({'u200d':['u0f41','u0f42']})]}
    for name,lookups in configs.items():
        path=folder/(name+'.ttf');make_font(path)
        with TTFont(path,recalcTimestamp=False) as font:
            del font['GPOS'];font['GSUB']=newTable('GSUB')
            font['GSUB'].table=layout('GSUB',[('ccmp',list(range(len(lookups))))],lookups,{'DFLT':script(language([0]))})
            font.save(path)

def generate(folder):
    folder.mkdir(parents=True,exist_ok=False)
    for path in build(folder):
        with TTFont(path,recalcTimestamp=False) as font:
            for kind in ('GSUB','GPOS'):
                if kind in font:
                    for record in font[kind].table.ScriptList.ScriptRecord:record.ScriptTag='DFLT'
            font.save(path)
    controls=[0xad,0x34f,0x61c,*range(0x180b,0x1810),*range(0x200b,0x2010),
        *range(0x202a,0x202f),*range(0x2060,0x2070),0xfeff,0xfe00,0xe0100,0xe01ef,0xe0001]
    cases={tuple(c) for c in CASES}
    for c in controls:
        for s in ([c],[0xf40,c],[c,0xf72],[c,0x200d],[0xf40,c,0xf40],[0xf40,0xf90,c],[c,0xfe00],[c,c]):cases.add(tuple(s))
    (folder/'cases.json').write_text(json.dumps(sorted(cases)))
    path=folder/'control-inventory.ttf';make_font(path)
    with TTFont(path,recalcTimestamp=False) as f:
        del f['GSUB'];del f['GPOS']
        for table in f['cmap'].tables:
            if table.isUnicode() and table.format!=14:
                for i,scalar in enumerate(controls):
                    if scalar<=65535 or table.format in (12,13):table.cmap[scalar]=f'u{0xf10+i:04x}'
        f.save(path)
    extra_fonts(folder)

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('folder',type=Path)
    generate(p.parse_args().folder)
