"""Isolate cmap eligibility in chained-context participation; QA observations."""
import argparse,json,subprocess
from pathlib import Path
from fontTools.ttLib import TTFont
from feature_plan_oracle import identity
from feature_plan_oracle import layout,language,script
from gsub_execution_oracle import ligature

def run(source,out,exe,native=None,ligature_mode=False):
    out.mkdir(parents=True,exist_ok=True);observations=[]
    # The same mark glyph and layout tables; only its encoded characters differ.
    for name,scalars in [('ascii',[ord('m')]),('combining',[0x301]),('unmapped',[]),('currency',[0x20ac]),('arabic',[0x627]),('both',[ord('m'),0x301])]:
        path=out/(name+'.ttf')
        with TTFont(source,recalcTimestamp=False) as f:
            if ligature_mode:f['GSUB'].table=layout('GSUB',[('ccmp',[0])],[ligature('AM')],{'latn':script(language([0]))})
            for table in f['cmap'].tables:
                if table.isUnicode():
                    table.cmap={cp:g for cp,g in table.cmap.items() if g!='M'}
                    table.cmap.update({cp:'M' for cp in scalars})
            f.save(path)
        mask=None
        if native:
            r=subprocess.run([str(native.resolve()),str(path.resolve())],capture_output=True,text=True,timeout=15);assert r.returncode==0,r.stdout;mask=json.loads(r.stdout);assert mask['status']==0
        for text in ['a','ab','ac']:
            c=[str(exe.resolve()),str(path.resolve()),','.join(f'{ord(x):x}' for x in text),'default','1148846080','0','0','en-US','layout']
            r=subprocess.run(c,capture_output=True,text=True,timeout=15);assert r.returncode==0,(name,r.stdout)
            value=json.loads(r.stdout)
            if mask:
                with TTFont(path) as f:
                    cmap=f.getBestCmap();glyph=f.getGlyphID(cmap[ord(text[0])]);simple=glyph!=0 and not ((mask['bits'][glyph//64]>>(glyph%64))&1)
                assert simple==bool(value['text_simple']),(name,text,mask,value)
            observations.append(dict(font=identity(path),variant=name,text=text,result=value,native=mask))
    data=dict(source=identity(source),reference=identity(exe),ligature_mode=ligature_mode,cases=observations);(out/'observations.json').write_text(json.dumps(data,indent=2)+'\n')
    print(json.dumps([dict(variant=c['variant'],text=c['text'],simple=c['result']['text_simple'],length=c['result']['complexity_length']) for c in observations],indent=2))
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('source',type=Path);p.add_argument('out',type=Path);p.add_argument('--exe',type=Path,required=True);p.add_argument('--native',type=Path);p.add_argument('--ligature',action='store_true');a=p.parse_args();run(a.source,a.out,a.exe,a.native,a.ligature)
