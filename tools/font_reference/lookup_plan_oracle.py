"""MaiCord Latin full-shaping plan corpus, with separate TextLayout observations."""
import argparse,json,struct,subprocess,sys
from pathlib import Path
from fontTools.ttLib import TTFont
from feature_plan_oracle import generate as generate_policy,identity
def run(root,exe,header):
    folder=root/'fixtures';generate_policy(folder);manifest=json.loads((folder/'fixtures.json').read_text());entries=[];observations=[];blobs=[]
    word=lambda *x:struct.pack('<'+'I'*len(x),*x)
    for c in manifest['cases']:
        p=folder/(c['name']+'.ttf')
        # Make selection/order probes active through the app's ordinary features.
        if c['name'].startswith(('script-','language-','lookup-order-test','shared-xxx')):
            with TTFont(p,recalcTimestamp=False) as f:
                for r in f[c['kind']].table.FeatureList.FeatureRecord:r.FeatureTag='ccmp' if c['kind']=='GSUB' else 'kern'
                f.save(p)
        with TTFont(p) as f:
            f.saveXML(folder/(c['name']+'.ttx'));layout=f[c['kind']].table;records={s.ScriptTag:s.Script for s in layout.ScriptList.ScriptRecord};tag='latn' if 'latn' in records else 'DFLT' if 'DFLT' in records else None
            selected=records.get(tag);languages={r.LangSysTag:r.LangSys for r in selected.LangSysRecord} if selected else {};lang=languages.get('ENG ',selected.DefaultLangSys) if selected else None
            language_tag='ENG ' if 'ENG ' in languages else None;plans=[]
            for independent in [0,1]:
                defaults=set(['ccmp','locl','rlig','rclt','rvrn','liga','clig','calt'] if c['kind']=='GSUB' else ['dist','mark','mkmk','kern'])
                if independent:defaults=set(['ccmp','locl','rlig','rclt','rvrn','dist','mark','mkmk'])
                indices=set();seen=set()
                if lang:
                    if lang.ReqFeatureIndex!=0xffff:indices.update(layout.FeatureList.FeatureRecord[lang.ReqFeatureIndex].Feature.LookupListIndex)
                    for index in lang.FeatureIndex:
                        r=layout.FeatureList.FeatureRecord[index]
                        if r.FeatureTag in defaults and r.FeatureTag not in seen:indices.update(r.Feature.LookupListIndex);seen.add(r.FeatureTag)
                plans.append(sorted(indices))
        configs=[]
        for independent in [0,1]:
            for rtl in [0]:
                command=[str(exe.resolve()),str(p.resolve()),'61','liga=0,clig=0,kern=0' if independent else 'default','1148846080',str(rtl),'0','en-US','layout'];v=subprocess.run(command,capture_output=True,text=True,timeout=15)
                assert v.returncode==0,(c['name'],v.returncode,v.stdout);value=json.loads(v.stdout);assert value['hresult']==value['placement_hresult']==0 and len(value['glyphs'])==1 and value['clusters']==[0]
                observations.append(dict(name=c['name'],independent=independent,rtl=rtl,result=value));ids=plans[independent]
                assert value['layout_hresult']==0 and value['layout_bidi']==0 and len(value['layout_glyphs'])==1
                configs.append(word(independent,rtl,len(ids),value['glyphs'][0],*value['placement_bits'][0],value['text_simple'],value['layout_glyphs'][0],*value['layout_placement_bits'][0])+b''.join(struct.pack('<H',x) for x in ids))
        script_tag=int.from_bytes(tag.encode(),'big') if tag else 0;lang_tag=int.from_bytes(language_tag.encode(),'big') if language_tag else 0
        blobs.append(word(p.stat().st_size,0 if c['kind']=='GSUB' else 1,script_tag,lang_tag,int(lang is not None))+p.read_bytes()+b''.join(configs))
        entries.append(dict(name=c['name'],font=identity(p),kind=c['kind'],script=tag,language=language_tag,language_found=bool(lang),plans=plans))
    raw=b'MCPLN03\0'+word(len(blobs))+b''.join(blobs);root.mkdir(parents=True,exist_ok=True);(root/'corpus.bin').write_bytes(raw)
    (folder/'fixtures.json').write_text(json.dumps(dict(scope='App-mode transformed policy fixtures; identities after feature-tag edits',cases=entries),indent=2)+'\n')
    differences=[dict(name=c['name'],independent=c['independent'],result=c['result']) for c in observations if c['result']['glyphs']!=c['result']['layout_glyphs'] or c['result']['placement_bits']!=c['result']['layout_placement_bits']]
    (root/'observations.json').write_text(json.dumps(dict(executable=identity(exe),dwrite=identity(Path(observations[0]['result']['dwrite_module'])),cases=observations),indent=2)+'\n')
    (root/'layout-differences.json').write_text(json.dumps(dict(scope='TextAnalyzer versus TextLayout observations; the native one-scalar gate now has separate comparisons, while whole-app eligibility remains open',cases=differences),indent=2)+'\n')
    (root/'corpus.json').write_text(json.dumps(dict(scope='MaiCord Latin/en-US/normal and independent-letter full-shaping plan checks against TextAnalyzer; TextLayout observations are separate and may differ; other script profiles are not implied',cases=entries,corpus=identity(root/'corpus.bin')),indent=2)+'\n')
    header.write_text('// First-party full-shaping plan expectations and Windows TextAnalyzer output.\nconstexpr u8 plan_fixture[]{\n'+'\n'.join('    '+','.join(f'0x{x:02x}' for x in raw[i:i+24])+',' for i in range(0,len(raw),24))+'\n};\n')
    print(f'Exported {len(entries)} fonts, {len(observations)} full-shaping observations, {len(differences)} separate TextLayout differences, {len(raw)} bytes')
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('root',type=Path);p.add_argument('--exe',type=Path,required=True);p.add_argument('--header',type=Path,required=True);a=p.parse_args();run(a.root,a.exe,a.header)
