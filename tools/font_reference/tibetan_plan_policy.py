"""Bounded Tibetan feature observations. FontTools authors offline QA only."""
import argparse, copy, json, sys, threading, time
from pathlib import Path
from fontTools.ttLib import TTFont, newTable
from feature_plan_oracle import language, script, layout
from gsub_execution_oracle import make_font, lookup, single, ORDER, identity
from gpos_execution_oracle import adjust
from devanagari_plan_oracle import batch_glyphs
from myanmar_plan_policy import scenarios, extended_scenarios

CONTEXTS = {'base':[0xf40], 'stack':[0xf40,0xf92],
            'above':[0xf40,0xf72], 'below':[0xf40,0xf74],
            'compound':[0xf40,0xf73], 'modified':[0xf40,0xf7e],
            'deep':[0xf40,0xf92,0xfb2,0xf74], 'broken':[0xf72]}
CMAP = dict(zip((0xf40,0xf92,0xf72,0xf74,0xf73,0xf7e,0xfb2,0xf71,0x25cc),
                ('A','B','C','D','E','M','N','X','L')))

def cases(extended):
    for original in extended_scenarios() if extended else scenarios():
        case=copy.deepcopy(original)
        if 'scripts' in case:
            case['scripts']={('tibt' if tag=='mym2' else tag):value for tag,value in case['scripts'].items()}
            for value in case['scripts'].values():
                value['languages']={('TIB ' if tag=='BRM ' else tag):record for tag,record in value['languages'].items()}
            case['name']=case['name'].replace('BRM','TIB')
        yield case
    if extended:
        for kind in ('GSUB','GPOS'):
            roots=[lookup(1,single({'A':'B'})),lookup(1,single({'A':'C'}))] if kind=='GSUB' else [adjust('A',0,0,31),adjust('A',0,0,73)]
            features=[('ccmp',[0]),('locl',[1])] if kind=='GSUB' else [('dist',[0]),('mark',[1])]
            for tag in ('tibt','latn','DFLT','mym2'):
                for default in (False,True):
                    yield dict(name=f'{kind}-selection-{tag}-{int(default)}',kind=kind,features=features,lookups=roots,
                        scripts={tag:script(language([0]) if default else None,{'ENG ':language([1])})})
            for tag in ('ENG ','TIB '):
                yield dict(name=f'{kind}-locale-{tag.strip()}',kind=kind,features=features,lookups=roots,
                    scripts={'tibt':script(language([0]),{tag:language([1])})})
            for default in (False,True):
                yield dict(name=f'{kind}-priority-{int(default)}',kind=kind,features=features,lookups=roots,
                    scripts={'tibt':script(language([0]) if default else None),'latn':script(language([1])),'DFLT':script(language([1]))})

def write_font(path,case):
    make_font(path,dict(name=case['name'],lookups=[lookup(1,single({'A':'A'}))]))
    with TTFont(path,recalcTimestamp=False) as font:
        for cmap in font['cmap'].tables:
            if cmap.isUnicode():cmap.cmap.update(CMAP)
        for kind in ('GSUB','GPOS'):
            same=kind==case['kind'];font[kind]=newTable(kind)
            features=case['features'] if same else [('dist' if kind=='GPOS' else 'ccmp',[0])]
            roots=case['lookups'] if same else [adjust('A',0,0,0) if kind=='GPOS' else lookup(1,single({'A':'A'}))]
            scripts=case.get('scripts') if same else None
            font[kind].table=layout(kind,features,roots,scripts or {'tibt':script(language(range(len(features)),case.get('required',65535) if same else 65535))})
        font.save(path)

def worker(args):
    start=time.perf_counter();rows=[]
    for index,case in enumerate(cases(args.extended)):
        if index%4!=args.shard:continue
        path=args.output_dir/(case['name']+'.ttf');write_font(path,case)
        row=dict(name=case['name'],font=identity(path),observations=[]);rows.append(row)
        for independent in (False,True):
            results=batch_glyphs(args.reference,path,list(CONTEXTS.values()),seed='0f40',detailed=True,independent=independent,timeout=2)
            for (context,text),value in zip(CONTEXTS.items(),results):
                value['placement_bits']=value.pop('placements')
                row['observations'].append(dict(context=context,scalars=text,mode='liga=0,clig=0,kern=0' if independent else 'default',glyph_names=[ORDER[g] for g in value['glyphs']],result=value))
    result=dict(status='observed',reference=identity(args.reference),sources=[identity(Path(__file__)),identity(Path(__file__).with_name('myanmar_plan_policy.py'))],checks=rows,seconds=time.perf_counter()-start)
    (args.output_dir/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(len(rows)*len(CONTEXTS)*2,'observations',round(result['seconds'],3),'seconds');return 0

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--reference',type=Path,required=True)
    p.add_argument('--output-dir',type=Path,required=True);p.add_argument('--shard',type=int,choices=range(4),required=True)
    p.add_argument('--extended',action='store_true');p.add_argument('--worker',action='store_true',help=argparse.SUPPRESS);args=p.parse_args()
    if args.worker:return worker(args)
    args.output_dir.mkdir(parents=True,exist_ok=True);path=args.output_dir/'result.json'
    if path.exists():p.error('Preserve existing observations')
    path.write_text('{"status":"running"}')
    sys.path.insert(0,str(Path(__file__).resolve().parents[1]));from verify_native_text import run_check,CheckFailure
    try:r=run_check('tibetan-plan',[sys.executable,str(Path(__file__).resolve()),*sys.argv[1:],'--worker'],Path.cwd(),args.output_dir,5,threading.Event())
    except CheckFailure as error:r=error.row
    result=json.loads(path.read_text());result['execution']=r
    if r['exit']:result['status']=r['status']
    path.write_text(json.dumps(result,indent=2)+'\n');return r['exit']
if __name__=='__main__':raise SystemExit(main())
