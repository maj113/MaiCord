"""First-party probes for Arabic join-control glyph and placement stages."""
import argparse,json,struct,subprocess
from pathlib import Path
from fontTools.ttLib import TTFont,newTable
from arabic_joining_oracle import make
from feature_plan_oracle import layout,language,script
from gsub_execution_oracle import obj,lookup,single,context,ligature,identity
from gpos_execution_oracle import adjust

B='u0628';X=B+'_isol'
def scenarios():
    yield 'baseline',[],[]
    for tag in ['ccmp','rlig']:
        for target in ['u200c','u200d','space']:
            yield f'{tag}-single-{target}',[(tag,[0])],[lookup(1,single({target:X}))]
            yield f'{tag}-context-{target}',[(tag,[0])],[context([target,B],[(1,1)]),lookup(1,single({B:X}))]
            yield f'{tag}-ligature-{target}',[(tag,[0])],[ligature([target,B],X)]
            yield f'{tag}-reverse-ligature-{target}',[(tag,[0])],[ligature([B,target],X)]
            yield f'{tag}-expand-{target}',[(tag,[0])],[lookup(2,obj('MultipleSubst',mapping={target:[X,B+'_fina']}))]
    for target in ['u200c','u200d','space']:
        yield f'gpos-{target}',[],[adjust(target,13,17,19)]

def run(root,exe):
    root.mkdir(parents=True,exist_ok=True);cases=[]
    for name,features,lookups in scenarios():
        path=root/(name+'.ttf');order=make(path);font=TTFont(path,recalcTimestamp=False)
        font['GSUB'].table=layout('GSUB',features,[] if name.startswith('gpos') else lookups,{'arab':script(language(range(len(features))))})
        if name.startswith('gpos'):
            font['GPOS']=newTable('GPOS');font['GPOS'].table=layout('GPOS',[('dist',[0])],lookups,{'arab':script(language([0]))})
        font['GDEF']=newTable('GDEF');font['GDEF'].table=obj('GDEF',Version=0x10000,GlyphClassDef=obj('ClassDef',classDefs={n:3 if n=='u064e' else 1 for n in order}),AttachList=None,LigCaretList=None,MarkAttachClassDef=None);font.save(path)
        for control in [0x200c,0x200d]:
          texts=[[0x628,control,0x628]]
          if name=='baseline':texts += [[0x628,control,0x64e,0x628],[0x628,0x64e,control,0x64e,0x628],[control,0x64e,0x628],[0x628,control,control,0x628]]
          for scalars in texts:
            for rtl in [0,1]:
                p=subprocess.run([str(exe),str(path),','.join(f'{v:x}' for v in scalars),'default','1148846080',str(rtl),'0','en-US'],capture_output=True,text=True,timeout=15,creationflags=0x08000000)
                assert p.returncode==0,(name,p.stdout,p.stderr);result=json.loads(p.stdout);assert result['hresult']==result['placement_hresult']==0
                cases.append(dict(name=name,sample=len(cases),font=identity(path),scalars=scalars,options='default',result=result))
                if rtl:print(name,hex(control),[order[g] for g in result['glyphs']],result['clusters'],[[struct.unpack('<f',struct.pack('<I',v))[0] for v in row] for row in result['placement_bits']])
    (root/'observations.json').write_text(json.dumps(dict(reference=identity(exe),glyph_order=order,cases=cases),indent=2))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('--exe',type=Path,required=True);a=p.parse_args();run(a.root.resolve(),a.exe.resolve())
