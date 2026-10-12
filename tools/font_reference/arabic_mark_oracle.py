"""Expose Arabic mark ordering independently of a font's substitution rules."""
import argparse,itertools,json,random,subprocess
from pathlib import Path
from fontTools.ttLib import TTFont,newTable
import arabic_joining_oracle as joining
from feature_plan_oracle import layout,language,script
from gsub_execution_oracle import obj,identity

MARKS=[0x610,0x618,*range(0x64b,0x660),0x670,0x6dc,0x6e3,0x6e4,0x6e7,0x6e8,
       0x8ca,0x8cb,0x8cd,0x8ce,0x8cf,0x8d3,0x8f0,0x8f3,0x8f5,
       0x300,0x301,0x302,0x309,0x323,0x334,0x342,0x346,0x34a,0x34f,0x35d,0x20d0,0x20db,0x20e1,0x1dc0]
def run(root,exe):
    root.mkdir(parents=True,exist_ok=True);path=root/'marks.ttf';joining.SCALARS=[0x628]+MARKS;order=joining.make(path)
    font=TTFont(path,recalcTimestamp=False);font['GSUB'].table=layout('GSUB',[],[],{'arab':script(language([]))})
    font['GDEF']=newTable('GDEF');font['GDEF'].table=obj('GDEF',Version=0x10000,GlyphClassDef=obj('ClassDef',classDefs={f'u{cp:04x}':3 for cp in MARKS}),AttachList=None,LigCaretList=None,MarkAttachClassDef=None);font.save(path)
    inputs=[[0x628,*pair,0x628] for pair in itertools.product(MARKS,repeat=2)]
    rng=random.Random(20260910);inputs += [[0x628,*rng.choices(MARKS,k=8),0x628] for _ in range(64)]
    cases=[]
    for scalars in inputs:
        p=subprocess.run([str(exe),str(path),','.join(f'{v:x}' for v in scalars),'default','1148846080','1','0','en-US'],capture_output=True,text=True,timeout=15,creationflags=0x08000000)
        assert p.returncode==0,(scalars,p.stdout,p.stderr);result=json.loads(p.stdout);assert result['hresult']==result['placement_hresult']==0
        cases.append(dict(scalars=scalars,result=result))
    (root/'observations.json').write_text(json.dumps(dict(reference=identity(exe),font=identity(path),glyph_order=order,cases=cases),indent=2));print(len(cases),'mark ordering observations')

def fixture(root,destination):
    data=json.loads((root/'observations.json').read_text());words=[]
    for c in data['cases']:
        names=[data['glyph_order'][g] for g in c['result']['glyphs']]
        assert all(n.startswith('u') and '_' not in n for n in names)
        expected=[int(n[1:],16) for n in names]
        words += [len(c['scalars']),len(expected),*c['scalars'],*expected]
    destination.write_text('// Windows Arabic mark order; CGJ removal is projected out in this ordering-only test.\nconstexpr u32 arabic_mark_cases='+str(len(data['cases']))+';\nconstexpr u32 arabic_mark_vectors[]{\n'+''.join(','.join(hex(v) for v in words[i:i+16])+',\n' for i in range(0,len(words),16))+'};\n')
    print(len(data['cases']),'mark fixtures')

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('--exe',type=Path);p.add_argument('--fixture',type=Path);a=p.parse_args()
    if a.fixture:fixture(a.root,a.fixture)
    else:
        if not a.exe:p.error('--exe is required for capture')
        run(a.root.resolve(),a.exe.resolve())
