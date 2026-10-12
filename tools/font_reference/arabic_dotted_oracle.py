"""First-party probes of Arabic dotted-circle insertion and stage timing (QA)."""
import argparse,json,subprocess
from pathlib import Path
from fontTools.ttLib import TTFont,newTable
import arabic_joining_oracle as joining
from arabic_mark_oracle import MARKS
from feature_plan_oracle import layout,language,script
from gsub_execution_oracle import obj,lookup,single,context,ligature,identity

B=0x628;F=0x64e;S=0x651;C=0x25cc

def texts():
    for mark in MARKS:
        for prefix in [[],[32],[0xa0],[0x60c],[0x200c],[0x200d],[B]]:
            yield prefix+[mark,B]
        yield [mark,mark,B]
        yield [mark,S,B]
        yield [S,mark,B]
        yield [mark]
    yield from [[F,0x34f,S,B],[F,0x200c,S,B],[F,0x200d,S,B],
        [S,F,F,B],[F,S,F,B],[F,S,32,B],[F,S,0xa0,B],[F,S,C,B],
        [B,32,F,S,B],[B,F,32,S,B],[B,F,F,B],[F]*16+[B]]

def run(root,exe):
    root.mkdir(parents=True,exist_ok=True);cases=[];rejected=[];orders={}
    joining.SCALARS=list(dict.fromkeys([B,0xa0,0x60c,0x200c,0x200d,C]+MARKS))
    scenarios=[('baseline',[],[]),('no-circle',[],[]),('base-classes',[],[])]
    for tag in ['ccmp','rlig']:
        scenarios += [(tag+'-single',[(tag,[0])],[lookup(1,single({'u25cc':'u0628_isol'}))]),
            (tag+'-context',[(tag,[0])],[context(['u25cc','u064e'],[(1,1)]),lookup(1,single({'u064e':'u0628_fina'}))]),
            (tag+'-ligature',[(tag,[0])],[ligature(['u25cc','u064e'],'u0628_medi')])]
    for name,features,lookups in scenarios:
        path=root/(name+'.ttf');order=joining.make(path);font=TTFont(path,recalcTimestamp=False)
        font['GSUB'].table=layout('GSUB',features,lookups,{'arab':script(language(range(len(features))))})
        font['GDEF']=newTable('GDEF');font['GDEF'].table=obj('GDEF',Version=0x10000,GlyphClassDef=obj('ClassDef',classDefs={n:3 if n.startswith('u') and '_' not in n and int(n[1:],16) in MARKS and name!='base-classes' else 1 for n in order}),AttachList=None,LigCaretList=None,MarkAttachClassDef=None)
        if name=='no-circle':
            for table in font['cmap'].tables:
                table.cmap.pop(C,None)
        font.save(path);orders[name]=order
        samples=list(texts()) if name=='baseline' else [[F,B],[S,F,B],[F,S,B],[32,F,B],[0x200d,F,B],[F],[F,F,B]]
        for scalars in samples:
            p=subprocess.run([str(exe),str(path),','.join(f'{v:x}' for v in scalars),'default','1148846080','1','0','en-US'],capture_output=True,text=True,timeout=15,creationflags=0x08000000)
            result=json.loads(p.stdout)
            case=dict(name=name,sample=len(cases),font=identity(path),scalars=scalars,options='default',result=result)
            if result['hresult'] or result['placement_hresult']:rejected.append(case);continue
            assert p.returncode==0,(name,p.stdout,p.stderr);cases.append(case)
    (root/'observations.json').write_text(json.dumps(dict(reference=identity(exe),glyph_orders=orders,cases=cases,rejected=rejected),indent=2))
    print(len(cases),'captured;',len(rejected),'rejected multi-script or invalid calls')
    for case in cases:
        if case['scalars'] in [[F],[F,B],[S,F,B],[F,S,B],[32,F,B],[0x200d,F,B]]:
            print(case['name'],case['scalars'],'script',case['result']['script'],[orders[case['name']][g] for g in case['result']['glyphs']],case['result']['clusters'])

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('--exe',type=Path,required=True);a=p.parse_args();run(a.root.resolve(),a.exe.resolve())
