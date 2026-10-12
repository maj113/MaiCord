"""First-party Myanmar feature probes; fontTools is offline QA only.

Observe feature activation/order and language selection independently of the
native implementation. Batched contexts share one Windows font/analyzer.
"""
import argparse, json, sys, threading, time
from pathlib import Path
from fontTools.ttLib import TTFont, newTable
from feature_plan_oracle import language, script, layout
from gsub_execution_oracle import make_font, lookup, single, marker, ORDER, identity
from gpos_execution_oracle import adjust
from devanagari_plan_oracle import batch_glyphs

SUBSTITUTION = ('locl','ccmp','rphf','pref','blwf','pstf','pres','abvs','blws','psts',
                'clig','liga','calt','rclt','rlig','rvrn','nukt','akhn','abvf','half','init','isol','cjct','vatu')
POSITION = ('curs','dist','kern','mark','abvm','blwm','mkmk')
CONTEXTS = {'base':[0x1000], 'kinzi':[0x1004,0x103a,0x1039,0x1000],
            'pre':[0x1000,0x1031], 'medial':[0x1000,0x103c],
            'stack':[0x1000,0x1039,0x1000], 'below':[0x1000,0x102f,0x1036],
            'post':[0x1000,0x102c], 'broken':[0x1031]}
CMAP = dict(zip((0x1000,0x1004,0x103a,0x1039,0x1031,0x103c,0x102f,0x1036,0x102c,0x103b,0x25cc),
                ('A','B','C','D','E','M','N','X','Y','Z','L')))

def scenarios():
    yield dict(name='control',kind='GSUB',features=[('test',[0])],lookups=[marker()])
    for kind,tags in [('GSUB',SUBSTITUTION),('GPOS',POSITION)]:
        for tag in tags:
            yield dict(name=kind+'-'+tag,kind=kind,features=[(tag,[0])],
                       lookups=[marker() if kind=='GSUB' else adjust('A',0,0,31)])
    active = ('locl','ccmp','rphf','pref','blwf','pstf','pres','abvs','blws','psts','clig','rclt')
    for a,b in zip(active,active[1:]):
        for reverse in (False,True):
            yield dict(name=f'order-{a}-{b}-{int(reverse)}',kind='GSUB',
                       features=sorted([(a,[int(reverse)]),(b,[int(not reverse)])]),
                       lookups=[lookup(1,single({'A':'B'})),lookup(1,single({'B':'C'}))])
        yield dict(name=f'shared-{a}-{b}',kind='GSUB',features=sorted([(a,[0]),(b,[0])]),
                   lookups=[lookup(1,single(dict(zip(ORDER[1:-1],ORDER[2:]))))])
    for kind in ('GSUB','GPOS'):
        for tag in ('test','ccmp','clig') if kind=='GSUB' else ('test','kern'):
            yield dict(name=f'required-{kind}-{tag}',kind=kind,features=[(tag,[0])],required=0,
                       lookups=[lookup(1,single(dict(zip(ORDER[1:-1],ORDER[2:])))) if kind=='GSUB' else adjust('A',0,0,31)])
    for tag in ('ENG ','BRM '):
        for default in (True,False):
            yield dict(name=f'language-{tag.strip()}-{int(default)}',kind='GSUB',
                       features=[('ccmp',[0]),('locl',[1])],
                       lookups=[lookup(1,single({'A':'B'})),lookup(1,single({'A':'C'}))],
                       scripts={'mym2':script(language([0]) if default else None,{tag:language([1])})})
    for tag in ('mymr','DFLT','latn'):
        yield dict(name=f'script-{tag}',kind='GSUB',features=[('ccmp',[0])],lookups=[marker()],
                   scripts={tag:script(language([0]))})

def extended_scenarios():
    yield dict(name='GSUB-rkrf',kind='GSUB',features=[('rkrf',[0])],lookups=[marker()])
    yield dict(name='GSUB-haln',kind='GSUB',features=[('haln',[0])],lookups=[marker()])
    pairs=(('locl','nukt'),('nukt','akhn'),('akhn','rphf'),('pref','rkrf'),
           ('rkrf','abvf'),('abvf','half'),('half','vatu'),('vatu','cjct'),
           ('cjct','isol'),('isol','pres'),('pres','rvrn'),('rvrn','rlig'),('rlig','calt'),('pres','haln'))
    for a,b in pairs:
        for reverse in (False,True):
            yield dict(name=f'order-{a}-{b}-{int(reverse)}',kind='GSUB',
                       features=sorted([(a,[int(reverse)]),(b,[int(not reverse)])]),
                       lookups=[lookup(1,single({'A':'B'})),lookup(1,single({'B':'C'}))])
        yield dict(name=f'shared-{a}-{b}',kind='GSUB',features=sorted([(a,[0]),(b,[0])]),
                   lookups=[lookup(1,single(dict(zip(ORDER[1:-1],ORDER[2:]))))])
    for a,b in zip(POSITION,POSITION[1:]):
        yield dict(name=f'shared-pos-{a}-{b}',kind='GPOS',features=sorted([(a,[0]),(b,[0])]),lookups=[adjust('A',0,0,31)])
    yield dict(name='required-DFLT',kind='GSUB',features=[('test',[0])],
               lookups=[lookup(1,single(dict(zip(ORDER[1:-1],ORDER[2:]))))],scripts={'DFLT':script(language([0],0))})
    for default in (True,False):
        yield dict(name=f'language-script-priority-{int(default)}',kind='GSUB',features=[('ccmp',[0]),('locl',[1])],
                   lookups=[lookup(1,single({'A':'B'})),lookup(1,single({'A':'C'}))],
                   scripts={'DFLT':script(language([1])),'mym2':script(language([0]) if default else None)})

def write_font(path,case):
    make_font(path,dict(name=case['name'],lookups=[lookup(1,single({'A':'A'}))]))
    with TTFont(path,recalcTimestamp=False) as font:
        for cmap in font['cmap'].tables:
            if cmap.isUnicode():cmap.cmap.update(CMAP)
        for kind in ('GSUB','GPOS'):
            font[kind]=newTable(kind)
            same=kind==case['kind']
            features=case['features'] if same else [('dist' if kind=='GPOS' else 'ccmp',[0])]
            roots=case['lookups'] if same else [adjust('A',0,0,0) if kind=='GPOS' else lookup(1,single({'A':'A'}))]
            scripts=case.get('scripts') if same else None
            font[kind].table=layout(kind,features,roots,scripts or {'mym2':script(language(range(len(features)),case.get('required',65535) if same else 65535))})
        font.save(path)

def worker(args):
    start=time.perf_counter();rows=[]
    for index,case in enumerate(extended_scenarios() if args.extended else scenarios()):
        if index%4!=args.shard:continue
        path=args.output_dir/(case['name']+'.ttf');write_font(path,case)
        row=dict(name=case['name'],font=identity(path),observations=[]);rows.append(row)
        for independent in (False,True):
            results=batch_glyphs(args.reference,path,list(CONTEXTS.values()),seed='1000',detailed=True,independent=independent,timeout=2)
            for (context,text),value in zip(CONTEXTS.items(),results):
                value['placement_bits']=value.pop('placements')
                row['observations'].append(dict(context=context,scalars=text,mode='liga=0,clig=0,kern=0' if independent else 'default',
                    glyph_names=[ORDER[g] for g in value['glyphs']],result=value))
    result=dict(status='observed',reference=identity(args.reference),source=identity(Path(__file__)),checks=rows,seconds=time.perf_counter()-start)
    (args.output_dir/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(len(rows)*16,'observations',round(result['seconds'],3),'seconds');return 0

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--reference',type=Path,required=True)
    p.add_argument('--output-dir',type=Path,required=True);p.add_argument('--shard',type=int,choices=range(4),required=True)
    p.add_argument('--extended',action='store_true')
    p.add_argument('--worker',action='store_true',help=argparse.SUPPRESS);args=p.parse_args()
    if args.worker:return worker(args)
    args.output_dir.mkdir(parents=True,exist_ok=True);path=args.output_dir/'result.json'
    if path.exists():p.error('Preserve existing observations')
    path.write_text('{"status":"running"}')
    sys.path.insert(0,str(Path(__file__).resolve().parents[1]));from verify_native_text import run_check,CheckFailure
    try:r=run_check('myanmar-plan',[sys.executable,str(Path(__file__).resolve()),*sys.argv[1:],'--worker'],Path.cwd(),args.output_dir,5,threading.Event())
    except CheckFailure as error:r=error.row
    result=json.loads(path.read_text());result['execution']=r
    if r['exit']:result['status']=r['status']
    path.write_text(json.dumps(result,indent=2)+'\n');return r['exit']
if __name__=='__main__':raise SystemExit(main())
