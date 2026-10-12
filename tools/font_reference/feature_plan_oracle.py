"""First-party script/language/feature policy probes. Windows observations, QA only."""
import argparse,copy,json,platform,struct,subprocess,sys
from pathlib import Path
from fontTools.ttLib import TTFont,newTable
from gsub_execution_oracle import obj,lookup,single,make_font,ORDER,TEXT,identity
from gpos_execution_oracle import adjust

TAGS='ccmp locl rlig liga clig calt rclt rvrn kern mark mkmk dist curs dlig hlig salt ss01 aalt frac lnum tnum onum pnum case cpsp zero ordn subs sups cv01 swsh cswh hist rand rtlm rtla ltra ltrm vert vrt2 vkrn vhal halt palt vpal fwid hwid pkna nalt unic titl test'.split()
def language(indices=(),required=0xffff):return obj('LangSys',LookupOrder=None,ReqFeatureIndex=required,FeatureCount=len(indices),FeatureIndex=list(indices))
def script(default=None,languages=None):return dict(default=default,languages=languages or {})
def layout(kind,features,lookups,scripts):
    records=[]
    for tag,s in sorted(scripts.items()):
        languages=[obj('LangSysRecord',LangSysTag=t,LangSys=v) for t,v in sorted(s['languages'].items())]
        records.append(obj('ScriptRecord',ScriptTag=tag,Script=obj('Script',DefaultLangSys=s['default'],LangSysCount=len(languages),LangSysRecord=languages)))
    return obj(kind,Version=0x10000,ScriptList=obj('ScriptList',ScriptCount=len(records),ScriptRecord=records),FeatureList=obj('FeatureList',FeatureCount=len(features),FeatureRecord=[obj('FeatureRecord',FeatureTag=tag,Feature=obj('Feature',FeatureParams=None,LookupCount=len(ids),LookupListIndex=ids)) for tag,ids in features]),LookupList=obj('LookupList',LookupCount=len(lookups),Lookup=lookups))
def scenarios():
    cases=[]
    def add(name,kind,features,lookups,options=('default',),scripts=None,locales=('en-US',),purpose=''):
        if scripts is None:scripts={tag:script(language(range(len(features)))) for tag in ['DFLT','latn']}
        cases.append(dict(name=name,kind=kind,features=features,lookups=lookups,scripts=scripts,options=options,locales=locales,purpose=purpose))
    for kind in ['GSUB','GPOS']:
        for tag in TAGS:
            add(f'default-{kind.lower()}-{tag}',kind,[(tag,[0])],[lookup(1,single({'A':'B'})) if kind=='GSUB' else adjust('A',13,17,19)],('default',tag+'=0',tag+'=1'),purpose='Independent default and explicit activation')
    pairs=[('ccmp','liga'),('locl','ccmp'),('rvrn','ccmp'),('rlig','liga'),('calt','liga'),('clig','liga'),('rclt','rlig'),('test','liga'),('xxx1','xxx2')]
    for left,right in pairs:
        for swapped in [False,True]:
            features=sorted([(left,[int(swapped)]),(right,[int(not swapped)])]);both=left+'=1,'+right+'=1'
            add(f'order-{left}-{right}-{int(swapped)}','GSUB',features,[lookup(1,single({'A':'B'})),lookup(1,single({'B':'C'}))],('default',both,right+'=1,'+left+'=1'),purpose='Feature stages versus global lookup order and option order')
    for tag in ['test','liga','ccmp','rlig']:
        for optional in [False,True]:
            scripts={t:script(language([0] if optional else [],0)) for t in ['DFLT','latn']}
            add(f'required-{tag}-{int(optional)}','GSUB',[(tag,[0])],[lookup(1,single({'A':'B','B':'C'}))],('default',tag+'=0',tag+'=1'),scripts,purpose='Required feature activation and optional-list duplication')
    for tag in ['test','ccmp','liga']:
        for indices in [[0,1],[1,0],[0,0]]:
            add(f'lookup-order-{tag}-'+''.join(map(str,indices)),'GSUB',[(tag,indices)],[lookup(1,single({'A':'B','B':'C'})),lookup(1,single({'B':'D'}))],(tag+'=1',),purpose='Lookup sorting and repetition inside one feature')
    for tags in [('ccmp','liga'),('liga','clig'),('ccmp','locl'),('xxx1','xxx2')]:
        add('shared-'+'-'.join(tags),'GSUB',[(t,[0]) for t in sorted(tags)],[lookup(1,single({'A':'B','B':'C','C':'D'}))],('default',','.join(t+'=1' for t in tags)),purpose='Lookup deduplication across features/stages')
    choices={t:lookup(1,single({'A':g})) for t,g in [('DFLT','X'),('latn','Y'),('cyrl','Z'),('grek','B')]}
    for selected in [('DFLT',),('latn',),('cyrl',),('DFLT','latn'),('cyrl','grek')]:
        features=[('test',[i]) for i in range(len(selected))];scripts={t:script(language([i])) for i,t in enumerate(selected)}
        add('script-'+'-'.join(selected),'GSUB',features,[choices[t] for t in selected],('test=1',),scripts,purpose='Script fallback for Latin text')
    locales=['en-US','en-GB','tr-TR','az-Latn-AZ','sr-Latn-RS','sr-Cyrl-RS','de-DE','nl-NL','und','zz-ZZ']
    forms=['X','Y','Z','B','C','D'];langtags=['ENG ','TRK ','SRB ','DEU ','NLD ']
    features=[('test',[i]) for i in range(6)];stages=[lookup(1,single({'A':g})) for g in forms]
    for default in [False,True]:
        scripts={'latn':script(language([0]) if default else None,{tag:language([i+1]) for i,tag in enumerate(langtags)}),'DFLT':script(language([0]))}
        add('language-default-'+str(int(default)),'GSUB',features,stages,('test=1',),scripts,locales,purpose='Locale to language tag selection, default LangSys fallback')
    for language_order in [[0,1],[1,0],[0,0,1]]:
        scripts={t:script(language(language_order)) for t in ['DFLT','latn']}
        add('language-feature-order-'+''.join(map(str,language_order)),'GSUB',[('test',[0]),('test',[1])],[lookup(1,single({'A':'B','B':'C'})),lookup(1,single({'B':'D'}))],('test=1',),scripts,purpose='Feature-index order, duplicate tags and repeated LangSys indices')
    alternate=lookup(3,obj('AlternateSubst',alternates={'A':['B','C','D']}))
    for tag in ['salt','test','rlig']:
        add('parameter-'+tag,'GSUB',[(tag,[0])],[alternate],('default',*[tag+'='+str(i) for i in range(5)],tag+'=1,'+tag+'=2',tag+'=2,'+tag+'=1'),purpose='Alternate parameters, overflow and repeated options')
    for tags in [('aalt','salt'),('ccmp','salt')]:
        for reverse in [False,True]:
            scripts={t:script(language([1,0] if reverse else [0,1])) for t in ['DFLT','latn']}
            add('parameter-shared-'+'-'.join(tags)+'-'+str(int(reverse)),'GSUB',[(t,[0]) for t in tags],[alternate],(tags[0]+'=2,'+tags[1]+'=3',tags[1]+'=3,'+tags[0]+'=2',tags[1]+'=2','default'),scripts,purpose='Conflicting parameters on one shared lookup')
    for required in [0,1]:
        scripts={t:script(language([1-required],required)) for t in ['DFLT','latn']}
        add('required-global-order-'+str(required),'GSUB',[('ccmp',[0]),('test',[1])],[lookup(1,single({'A':'B'})),lookup(1,single({'B':'C'}))],('test=1',),scripts,purpose='Required lookup ordering relative to optional lookups')
    add('parameter-shared-liga-salt','GSUB',[('liga',[0]),('salt',[0])],[alternate],('default','salt=2','liga=0,salt=2','salt=3,liga=2','liga=2,salt=3'),purpose='Default optional feature priority on a shared lookup')
    scripts={t:script(language([0],1)) for t in ['DFLT','latn']}
    add('parameter-required-test-salt','GSUB',[('salt',[0]),('test',[0])],[alternate],('default','salt=2','test=3','salt=2,test=3','test=3,salt=2'),scripts,purpose='Required-feature parameter priority')
    return cases
def generate(folder):
    folder.mkdir(parents=True,exist_ok=True);manifest=[]
    for c in scenarios():
        path=folder/(c['name']+'.ttf');make_font(path,dict(name=c['name'],lookups=[lookup(1,single({'A':'A'}))]))
        with TTFont(path,recalcTimestamp=False) as font:
            del font['GSUB'];font[c['kind']]=newTable(c['kind']);font[c['kind']].table=layout(c['kind'],c['features'],c['lookups'],c['scripts']);font.save(path)
        with TTFont(path) as font:font.saveXML(folder/(c['name']+'.ttx'))
        manifest.append({k:v for k,v in c.items() if k not in ['lookups','scripts']}|dict(font=identity(path),text=TEXT['A'],script_records={t:dict(default=None if s['default'] is None else dict(features=s['default'].FeatureIndex,required=s['default'].ReqFeatureIndex),languages={k:dict(features=v.FeatureIndex,required=v.ReqFeatureIndex) for k,v in s['languages'].items()}) for t,s in c['scripts'].items()}))
    (folder/'fixtures.json').write_text(json.dumps(dict(scope='First-party Latin-text script/language/default-feature and ordering probes',cases=manifest),indent=2)+'\n');print(f'Generated {len(manifest)} feature-policy fonts')
def record(folder,exe,out):
    manifest=json.loads((folder/'fixtures.json').read_text());observations=[];module=None
    for c in manifest['cases']:
        path=folder/(c['name']+'.ttf');assert identity(path)['sha256']==c['font']['sha256']
        for locale in c['locales']:
            for options in c['options']:
                for rtl in [0,1]:
                    command=[str(exe.resolve()),str(path.resolve()),','.join(f'{ord(x):x}' for x in c['text']),options,'1148846080',str(rtl),'0',locale]
                    run=subprocess.run(command,capture_output=True,text=True,timeout=15)
                    if run.returncode:raise RuntimeError((c['name'],locale,options,rtl,run.returncode,run.stdout,run.stderr))
                    v=json.loads(run.stdout);assert v['hresult']==v['placement_hresult']==0
                    v['glyph_names']=[ORDER[g] for g in v['glyphs']];v['placement_values']=[[struct.unpack('<f',struct.pack('<I',x))[0] for x in row] for row in v['placement_bits']]
                    observations.append(dict(name=c['name'],locale=locale,options=options,rtl=rtl,result=v));loaded=Path(v['dwrite_module']).resolve()
                    if module is None:module=loaded
                    assert loaded==module
    out.parent.mkdir(parents=True,exist_ok=True);out.write_text(json.dumps(dict(scope='Installed Windows Latin feature/script/language policy; no native planner conformance claim',os=platform.platform(),windows_version=list(sys.getwindowsversion()),executable=identity(exe),dwrite=identity(module),fixtures=identity(folder/'fixtures.json'),observations=observations),indent=2)+'\n');print(f'Recorded {len(observations)} feature-policy observations')
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);sub=p.add_subparsers(dest='mode',required=True);g=sub.add_parser('generate');g.add_argument('folder',type=Path);r=sub.add_parser('record');r.add_argument('folder',type=Path);r.add_argument('--exe',type=Path,required=True);r.add_argument('--out',type=Path,required=True);a=p.parse_args()
    if a.mode=='generate':generate(a.folder)
    else:record(a.folder,a.exe,a.out)
