"""First-party probes of MaiCord's Arabic script feature schedule (QA only)."""
import argparse,itertools,json,struct,subprocess
from pathlib import Path
from fontTools.ttLib import TTFont,newTable
from arabic_joining_oracle import make
from embed_fixture import write_embedded_array
from feature_plan_oracle import layout,language,script,TAGS
from gsub_execution_oracle import obj,lookup,single,context,ligature,identity
from gpos_execution_oracle import adjust

B='u0628';X=B+'_isol';Y=B+'_fina';Z=B+'_medi'
COMMON='ccmp locl rvrn rlig rclt calt liga clig mset isol'.split()
OPTIONS=['default','liga=0,clig=0,kern=0']
def scenarios():
    for kind in ['GSUB','GPOS']:
        for tag in sorted(set(TAGS+'isol fina medi init mset'.split())):
            yield dict(name=f'default-{kind}-{tag}',kind=kind,features=[(tag,[0])],lookups=[lookup(1,single({B:X})) if kind=='GSUB' else adjust(B,13,17,19)],texts=[[0x628],[0x628]*3])
    for left,right in itertools.combinations(COMMON,2):
        for swapped in [False,True]:
            yield dict(name=f'order-{left}-{right}-{int(swapped)}',kind='GSUB',features=sorted([(left,[int(swapped)]),(right,[int(not swapped)])]),lookups=[lookup(1,single({B:X})),lookup(1,single({X:Y}))],texts=[[0x628]])
        yield dict(name=f'shared-{left}-{right}',kind='GSUB',features=sorted([(left,[0]),(right,[0])]),lookups=[lookup(1,single({B:X,X:Y,Y:Z}))],texts=[[0x628]])
    for form in ['fina','medi','init']:
        for common in COMMON[:-1]:
            for swapped in [False,True]:
                yield dict(name=f'form-order-{common}-{form}-{int(swapped)}',kind='GSUB',features=sorted([(common,[int(swapped)]),(form,[int(not swapped)])]),lookups=[lookup(1,single({B:X})),lookup(1,single({X:Y}))],texts=[[0x628]*3])
            yield dict(name=f'form-shared-{common}-{form}',kind='GSUB',features=sorted([(common,[0]),(form,[0])]),lookups=[lookup(1,single({B:X,X:Y,Y:Z}))],texts=[[0x628]*3])
    for tags in [('arab',),('DFLT',),('latn',),('arab','DFLT'),('DFLT','latn'),('cyrl',),('grek','latn'),('cyrl','grek')]:
        yield dict(name='script-'+'-'.join(tags),kind='GSUB',features=[('ccmp',[i]) for i in range(len(tags))],lookups=[lookup(1,single({B:g})) for g in [X,Y,Z][:len(tags)]],scripts={tag:script(language([i])) for i,tag in enumerate(tags)},texts=[[0x628],[0x628]*3])
    for default in [False,True]:
        yield dict(name='language-'+str(int(default)),kind='GSUB',features=[('ccmp',[i]) for i in range(3)],lookups=[lookup(1,single({B:g})) for g in [X,Y,Z]],scripts={'arab':script(language([0]) if default else None,{'ENG ':language([1]),'ARA ':language([2])})},texts=[[0x628]])
    for required_tag in ['test','ccmp','isol','rlig']:
        for optional in [False,True]:
            yield dict(name=f'required-{required_tag}-{int(optional)}',kind='GSUB',features=[(required_tag,[0])],lookups=[lookup(1,single({B:X,X:Y,Y:Z,Z:B+'_init',B+'_init':'u0627_isol'}))],scripts={'arab':script(language([0] if optional else [],0))},texts=[[0x628],[0x628]*3])
    indices={'isol':0,'init':2,'medi':3,'fina':4};text=[0x628,0x621,0x628,0x628,0x628];names=[B,'u0621',B,B,B]
    for left,right in itertools.combinations(indices,2):
        for swapped in [False,True]:
            li,ri=indices[left],indices[right];back=[];ahead=[]
            if li<ri:back=[X if i==li else names[i] for i in range(ri-1,li-1,-1)]
            else:ahead=[X if i==li else names[i] for i in range(ri+1,li+1)]
            first=lookup(1,single({B:X}));second=context([B],[(0,2)],chained=True,back=back,ahead=ahead)
            yield dict(name=f'forms-order-{left}-{right}-{int(swapped)}',kind='GSUB',features=sorted([(left,[int(swapped)]),(right,[int(not swapped)])]),lookups=([second,first] if swapped else [first,second])+[lookup(1,single({B:Y}))],texts=[text])
    for form in ['isol','fina','medi','init']:
        yield dict(name='composition-merge-'+form,kind='GSUB',features=sorted([('ccmp',[0]),(form,[1])]),lookups=[ligature([B,B],X),lookup(1,single({X:Y}))],texts=[[0x628]*2,[0x628]*3])
        yield dict(name='composition-expand-'+form,kind='GSUB',features=sorted([('ccmp',[0]),(form,[1])]),lookups=[lookup(2,obj('MultipleSubst',mapping={B:[X,Y]})),lookup(1,single({X:Z,Y:Z}))],texts=[[0x628],[0x628]*3])
        yield dict(name='composition-roundtrip-'+form,kind='GSUB',features=sorted([('ccmp',[0,1]),(form,[2])]),lookups=[ligature([B,B],X),lookup(2,obj('MultipleSubst',mapping={X:[B,B]})),lookup(1,single({B:Y}))],texts=[[0x628]*2,[0x628]*3])
        yield dict(name='composition-extra-'+form,kind='GSUB',features=sorted([('ccmp',[0,1]),(form,[2])]),lookups=[ligature([B,B],X),lookup(2,obj('MultipleSubst',mapping={X:[B,B,B]})),lookup(1,single({B:Y}))],texts=[[0x628]*2,[0x628]*3])

def run(root,exe):
    root.mkdir(parents=True,exist_ok=True);cases=[]
    for c in scenarios():
        path=root/(c['name']+'.ttf');order=make(path);font=TTFont(path,recalcTimestamp=False)
        del font['GSUB'];font[c['kind']]=newTable(c['kind']);font[c['kind']].table=layout(c['kind'],c['features'],c['lookups'],c.get('scripts',{'arab':script(language(range(len(c['features']))))}))
        if c['kind']=='GPOS':
            font['GSUB']=newTable('GSUB');font['GSUB'].table=layout('GSUB',[],[],{'arab':script(language([]))})
        font['GDEF']=newTable('GDEF');font['GDEF'].table=obj('GDEF',Version=0x10000,GlyphClassDef=obj('ClassDef',classDefs={n:1 for n in order}),AttachList=None,LigCaretList=None,MarkAttachClassDef=None)
        font.save(path)
        for scalars in c['texts']:
            for options in OPTIONS:
                p=subprocess.run([str(exe),str(path),','.join(f'{v:x}' for v in scalars),options,'1148846080','1','0','en-US'],capture_output=True,text=True,timeout=15,creationflags=0x08000000)
                assert p.returncode==0,(c['name'],scalars,options,p.stdout,p.stderr)
                result=json.loads(p.stdout);assert result['hresult']==result['placement_hresult']==0
                cases.append(dict(name=c['name'],kind=c['kind'],features=c['features'],font=identity(path),scalars=scalars,options=options,result=result))
    (root/'observations.json').write_text(json.dumps(dict(reference=identity(exe),glyph_order=order,cases=cases),indent=2))
    print(len(cases),'Arabic schedule observations')

def fixture(roots,destination,binary=False,symbol='arabic_plan_fixture',rtl=None,directed=False,exclude_scalars=()):
    cases=[];fonts=[];ids={}
    for root in roots:
        data=json.loads((root/'observations.json').read_text())
        for c in data['cases']:
            if rtl is not None and c['result']['rtl']!=rtl:continue
            if any(cp in exclude_scalars for cp in c['scalars']):continue
            path=Path(c['font']['path']);assert identity(path)==c['font'];key=c['font']['sha256']
            if key not in ids:ids[key]=len(fonts);fonts.append(path.read_bytes())
            result=c['result'];assert result['hresult']==result['placement_hresult']==0
            assert all(v<=65535 for v in c['scalars']) and len(result['clusters'])==len(c['scalars'])
            fields=[ids[key],int(c['options']!=OPTIONS[0]),len(c['scalars']),len(result['glyphs'])]
            if binary or directed:fields=[result['em_bits'],result['rtl']]+fields
            raw=struct.pack('<'+'I'*len(fields),*fields)+struct.pack('<'+'I'*len(c['scalars']),*c['scalars'])
            raw+=struct.pack('<'+'H'*len(result['glyphs']),*result['glyphs'])
            raw+=struct.pack('<'+'I'*len(result['clusters']),*result['clusters'])
            bits=[v for row in result['placement_bits'] for v in row];raw+=struct.pack('<'+'I'*len(bits),*bits);cases.append(raw)
    blob=(b'MCARPL3\0' if binary or directed else b'MCARPL1\0')+struct.pack('<2I',len(fonts),len(cases))
    for font in fonts:blob+=struct.pack('<I',len(font))+font
    blob+=b''.join(cases)
    if binary:destination.write_bytes(blob)
    else:
        assert symbol.isidentifier()
        label='shaping' if directed else 'scheduling'
        write_embedded_array(destination,'// First-party Arabic '+label+'; captured Windows expectations.\n','constexpr u8 '+symbol+'[]',blob)
    print(len(fonts),'fonts',len(cases),'native plan fixtures')

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('--exe',type=Path);p.add_argument('--fixture',type=Path);p.add_argument('--binary',type=Path);p.add_argument('--extra',type=Path,action='append',default=[]);p.add_argument('--symbol',default='arabic_plan_fixture');p.add_argument('--rtl',type=int,choices=[0,1]);p.add_argument('--directed',action='store_true');p.add_argument('--exclude-scalar',type=lambda v:int(v,0),action='append',default=[]);a=p.parse_args()
    if a.fixture or a.binary:fixture([a.root]+a.extra,a.binary or a.fixture,bool(a.binary),a.symbol,a.rtl,a.directed,a.exclude_scalar)
    else:
        if not a.exe:p.error('--exe is required for capture')
        run(a.root.resolve(),a.exe.resolve())
