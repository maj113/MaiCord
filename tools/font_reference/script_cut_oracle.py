"""First-party diagnostic fonts distinguish TextLayout script choice after font cuts."""
from pathlib import Path
import argparse,hashlib,json,os,struct,subprocess,sys
from fontTools.fontBuilder import FontBuilder
from fontTools.ttLib import TTFont,newTable
from fontTools.ttLib.tables._g_l_y_f import Glyph
from gsub_execution_oracle import obj,lookup,single
from feature_plan_oracle import layout,script,language

TAGS='DFLT latn arab hebr hani deva dev2 grek cyrl beng bng2 guru gur2 gujr gjr2 orya ory2 taml tml2 telu tel2 knda knd2 mlym mlm2 thai lao  tibt mymr mym2 khmr khm2 hang kana'.split()
TAGS=[t.ljust(4) for t in TAGS]
SCRIPTS=[('latin','a','b'),('arabic','\u0628','\u062a'),('hebrew','\u05d0','\u05d1'),('han','\u4e2d','\u6587'),('deva','\u0915','\u0937'),('greek','\u03b1','\u03b2'),('cyrillic','\u0410','\u0411'),('bengali','\u0995','\u09b7'),('gurmukhi','\u0a15','\u0a38'),('gujarati','\u0a95','\u0ab7'),('oriya','\u0b15','\u0b37'),('tamil','\u0b95','\u0bb7'),('telugu','\u0c15','\u0c37'),('kannada','\u0c95','\u0cb7'),('malayalam','\u0d15','\u0d37'),('thai','\u0e01','\u0e02'),('lao','\u0e81','\u0e82'),('tibetan','\u0f40','\u0f41'),('myanmar','\u1000','\u1001'),('khmer','\u1780','\u1781'),('hangul','\uac00','\ub098'),('hiragana','\u3042','\u3044'),('katakana','\u30a2','\u30a4')]
EMOJI=['\U0001f600','\u2764','\u00a9','1\ufe0f\u20e3','\U0001f600\u200d\U0001f469']
ORDER=['.notdef','text','emoji','space','control']+['script'+str(i) for i in range(len(TAGS))]
def identity(path):
    path=Path(path)
    with path.open('rb') as f:sha=hashlib.file_digest(f,'sha256').hexdigest()
    return dict(path=str(path),bytes=path.stat().st_size,sha256=sha)
def font(path,emoji,selected):
    b=FontBuilder(1000,isTTF=True);b.setupGlyphOrder(ORDER)
    cps={ord(c) for _,a,z in SCRIPTS for c in a+z}|{ord(c) for e in EMOJI for c in e}|set(map(ord,'a b()[]#*12.'))|{0xfe0e,0x1f3f4,0xe0067,0xe007f}
    cmap={cp:('emoji' if emoji and (cp in {0x1f600,0x2764,0xa9,0x31,0x1f469,0x1f3f4}) else 'text') for cp in cps}
    cmap[32]='space'
    for cp in (0x200d,0xfe0e,0xfe0f,0xe0067,0xe007f):cmap[cp]='control'
    b.setupCharacterMap(cmap);b.setupGlyf({g:Glyph() for g in ORDER});b.setupHorizontalMetrics({g:(0 if g=='control' else 600,0) for g in ORDER});b.setupHorizontalHeader(ascent=800,descent=-200)
    family='MaiCord Cut '+('Emoji' if emoji else 'Text');b.setupNameTable(dict(familyName=family,styleName='Regular',uniqueFontIdentifier=family+path.stem,fullName=family,psName=family.replace(' ','')))
    b.setupOS2(sTypoAscender=800,sTypoDescender=-200,usWinAscent=800,usWinDescent=200);b.setupPost();b.setupMaxp()
    if emoji:
        records=[('ccmp',[i]) for i in range(len(selected))];lookups=[lookup(1,single({'emoji':'script'+str(TAGS.index(tag))})) for tag in selected]
        b.font['GSUB']=newTable('GSUB');b.font['GSUB'].table=layout('GSUB',records,lookups,{tag:script(language([i])) for i,tag in enumerate(selected)})
    b.font['head'].created=b.font['head'].modified=2082844800;b.save(path)
    with TTFont(path) as check:assert check.getGlyphOrder()==ORDER
def prepare(root):
    root.mkdir(parents=True,exist_ok=True);fonts=root/'fonts';fonts.mkdir(exist_ok=True)
    font(fonts/'text.ttf',False,[])
    choices={'all':TAGS,'fallback':['DFLT','latn'],'default':['DFLT'],'latin':['latn'],'unrelated':['grek'],'absent':[]}
    for name,tags in choices.items():font(fonts/(name+'.ttf'),True,tags)
    cases=[]
    for emoji in EMOJI:
        for label,a,b in [('neutral','','')]+SCRIPTS:
            texts=[emoji] if not a else [a+emoji+b,emoji+a,a+emoji,a+' '+emoji+' '+b,'('+emoji+')'+a]
            for text in texts:
                for mode in ('whole','emoji','sizes'):
                    for version in choices:
                        cases.append(dict(id=len(cases),script=label,text=text,mode=mode,font=version,rtl=0))
    (root/'cases.json').write_text(json.dumps(dict(cases=cases,order=ORDER,tags=TAGS,font_inputs=[identity(p) for p in sorted(fonts.glob('*.ttf'))]),ensure_ascii=True,indent=2))
    print('Prepared',len(cases),'diagnostic cases')
def capture(root):
    data=json.loads((root/'cases.json').read_text());exe=root/'script-cut-reference.exe';env=os.environ.copy();env['LOCALAPPDATA']=str(root/'data');Path(env['LOCALAPPDATA']).mkdir(exist_ok=True)
    with (root/'observations.jsonl').open('w') as output:
        for c in data['cases']:
            args=[str(exe),str(root/'fonts/text.ttf'),str(root/'fonts'/(c['font']+'.ttf')),','.join(f'{ord(cp):x}' for cp in c['text']),c['mode'],str(c['rtl'])]
            p=subprocess.run(args,capture_output=True,text=True,env=env,creationflags=subprocess.CREATE_NO_WINDOW)
            row=dict(case=c,exit=p.returncode,result=json.loads(p.stdout) if p.stdout else None,stderr=p.stderr);output.write(json.dumps(row,ensure_ascii=True)+'\n')
            assert not p.returncode,row
            if c['id']%1000==0:print('Observed',c['id'],flush=True)
    (root/'reference-identity.json').write_text(json.dumps(identity(exe),indent=2));print('Captured',len(data['cases']))
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('action',choices=['prepare','capture']);p.add_argument('root',type=Path);a=p.parse_args()
    (prepare if a.action=='prepare' else capture)(a.root)
