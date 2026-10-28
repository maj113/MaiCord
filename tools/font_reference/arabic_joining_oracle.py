"""First-party fonts expose Windows Arabic form-feature selection."""
import argparse,itertools,json,subprocess
from pathlib import Path
from fontTools.fontBuilder import FontBuilder
from fontTools.ttLib import newTable
from fontTools.ttLib.tables._g_l_y_f import Glyph
from feature_plan_oracle import layout,language,script
from gsub_execution_oracle import lookup,single,identity

SCALARS=[0x628,0x627,0x621,0x640,0x64e,0x200c,0x200d]
FORMS=['isol','fina','medi','init']
def make(path):
    base={c:f'u{c:04x}' for c in SCALARS};order=['.notdef','space']+list(base.values())+[n+'_'+f for n in base.values() for f in FORMS]
    fb=FontBuilder(1000,isTTF=True);fb.setupGlyphOrder(order);fb.setupCharacterMap({32:'space',**base});glyphs={}
    for n in order:g=Glyph();g.numberOfContours=0;glyphs[n]=g
    fb.setupGlyf(glyphs);fb.setupHorizontalMetrics({n:(600,0) for n in order});fb.setupHorizontalHeader(ascent=800,descent=-200)
    fb.setupOS2(sTypoAscender=800,sTypoDescender=-200,usWinAscent=800,usWinDescent=200)
    fb.setupNameTable({'familyName':'MaiCord Arabic Joining QA','styleName':'Regular','fullName':'MaiCord Arabic Joining QA','psName':'MaiCordArabicJoiningQA','uniqueFontIdentifier':'MaiCord Arabic Joining QA'})
    fb.setupPost();fb.setupMaxp();font=fb.font;features=sorted((tag,[i]) for i,tag in enumerate(FORMS));lookups=[lookup(1,single({n:n+'_'+tag for n in base.values()})) for tag in FORMS]
    font['GSUB']=newTable('GSUB');font['GSUB'].table=layout('GSUB',features,lookups,{'arab':script(language(range(4)))})
    font['head'].created=font['head'].modified=3406620153;font.save(path);return order

def run(root,exe):
    root.mkdir(parents=True,exist_ok=True);path=root/'joining.ttf';order=make(path);cases=[]
    inputs=sorted(set(tuple(s) for n in [1,2,3] for s in itertools.product(SCALARS,repeat=n) if any(c in [0x628,0x627,0x621] for c in s)))
    inputs+=[(0x628,*([0x64e]*n),0x628) for n in [4,16,64]]
    for scalars in inputs:
        command=[str(exe),str(path),','.join(f'{c:x}' for c in scalars),'default','1148846080','1','0','en-US']
        p=subprocess.run(command,capture_output=True,text=True,timeout=15,creationflags=0x08000000);assert p.returncode==0,(scalars,p.stdout,p.stderr)
        result=json.loads(p.stdout);cases.append(dict(scalars=scalars,result=result))
    (root/'observations.json').write_text(json.dumps(dict(reference=identity(exe),font=identity(path),glyph_order=order,cases=cases),indent=2))
    print(len(cases),'Arabic form probes')
    for case in cases:
        if len(case['scalars'])<=2:print([hex(c) for c in case['scalars']],case['result'])

def fixture(source,destination):
    data=json.loads(source.read_text());order=data['glyph_order'];words=[];total=0
    for case in data['cases']:
        result=case['result'];assert result['hresult']==0 and result['placement_hresult']==0
        # This first-party font has no dotted circle. Windows inserts glyph 0
        # for a leading mark; join controls become space. Neither gets a form.
        names=[order[g] for g in result['glyphs'] if g!=0]
        assert len(names)==len(case['scalars']),(case,names)
        words.append(len(names));total+=len(names)
        for scalar,name in zip(case['scalars'],names):
            base=f'u{scalar:04x}';form=0
            if scalar in [0x200c,0x200d]:assert name=='space',(scalar,name)
            elif name!=base:
                assert name.startswith(base+'_'),(scalar,name)
                form=1<<FORMS.index(name[len(base)+1:])
            words.append(scalar|(form<<21))
    lines=['// Generated from first-party Windows observations by arabic_joining_oracle.py.',
           '// Reference SHA256: '+data['reference']['sha256'],
           '// Font SHA256: '+data['font']['sha256'],
           'constexpr u32 arabic_joining_case_count='+str(len(data['cases']))+';',
           'constexpr u32 arabic_joining_scalar_count='+str(total)+';',
           'constexpr u32 arabic_joining_vectors[]{']
    lines+=['    '+','.join(f'0x{v:x}' for v in words[i:i+12])+',' for i in range(0,len(words),12)]
    lines+=['};',''];destination.write_text('\n'.join(lines))
    print(len(data['cases']),'cases',total,'scalars exported')

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('--exe',type=Path);p.add_argument('--fixture',type=Path);a=p.parse_args()
    if a.fixture:fixture(a.root/'observations.json',a.fixture)
    else:
        if not a.exe:p.error('--exe is required for a capture')
        run(a.root.resolve(),a.exe.resolve())
