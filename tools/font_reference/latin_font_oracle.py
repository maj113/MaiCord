"""Actual-font Latin shaping observations; all shaping answers come from Windows."""
from pathlib import Path
import argparse,json,subprocess
from fontTools.ttLib import TTFont
from latin_preprocess_oracle import CASES,identity
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('root',type=Path);p.add_argument('--exe',required=True,type=Path);p.add_argument('--inventory',required=True,type=Path);a=p.parse_args();a.root.mkdir(parents=True,exist_ok=True)
    samples=CASES+['Človek Živjo čšž','café fiancée Ångström','Łódź Đorđe İstanbul','naïve coöperate résumé','Straße Æsir Œuvre','e\u0301cole A\u030angstro\u0308m','x\u0301y\u0327z','a\u0301\u0300b','AV To ffi fl','e\u0301 AV ffi  ']
    inventory=json.loads(a.inventory.read_text());rows=[];omitted=[]
    for font in inventory['rows']:
        path=Path(font['font']['path']);assert identity(path)==font['font']
        with TTFont(path) as f:coverage=set(f.getBestCmap())
        weights=[400,600] if font['family']=='Bahnschrift' else [0]
        for text in samples:
            # GetGlyphs is a single-face API; missing glyphs are intentionally
            # retained. Prefix/suffix provide an explicit Latin script context.
            text='a'+text+'b'
            for weight in weights:
                for independent in (0,1):
                    features='liga=0,clig=0,kern=0' if independent else 'default'
                    for bits in (1098907648,1099694080):
                        command=[str(a.exe),str(path),','.join(f'{ord(c):x}' for c in text),features,str(bits),'0','0','en-US']
                        # The layout option permits the reference to select the
                        # requested axis; capture both full shaping and layout.
                        if weight:command+=['layout',str(weight)]
                        run=subprocess.run(command,capture_output=True,text=True);r=json.loads(run.stdout)
                        if run.returncode:omitted.append(dict(font=font['font'],text=[ord(c) for c in text],result=r));continue
                        # Retain full single-font answers even if layout would
                        # substitute a fallback face for a missing character.
                        for key in list(r):
                            if key.startswith('layout_'):del r[key]
                        rows.append(dict(font=font['font'],text=[ord(c) for c in text],independent=independent,weight=weight,exit=run.returncode,result=r))
    (a.root/'latin-font-oracle.json').write_text(json.dumps(dict(executable=identity(a.exe),inventory=identity(a.inventory),rows=rows,omitted=omitted),indent=2))
    print(len(rows),'font cases;',len(omitted),'script-analysis rejections')
if __name__=='__main__':main()
