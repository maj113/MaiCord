"""Bounded authored Myanmar font-admission observations; offline QA only."""
import argparse,itertools,json,sys,threading,time
from pathlib import Path
from fontTools.ttLib import TTFont,newTable
from myanmar_input_font import make_font
from feature_plan_oracle import layout,script,language
from gsub_execution_oracle import lookup,single,identity
from gpos_execution_oracle import adjust
from devanagari_plan_oracle import batch_glyphs

POLICIES=('missing','empty-scripts','mym2','mym2-empty','mym2-no-language','mym2-eng','DFLT','latn','mymr')
CASES=[[0x1000,0x1031],[0x1031],[0x1004,0x103a,0x1039,0x1000,0x1031],
       [0x1000,0x1039,0x1000],[0x1000,0x103c],[0x1000,0x200d],[0x34f,0x34f],
       [0x200d,0x200c],[0x1000,0xfe00,0x1031],[0x1000,0x1031,0x1000],
       [0x1031,0x102d],[0x1000,0x102d],[0x1000,0x2060]]
def scripts(policy):
    if policy=='empty-scripts':return {}
    if policy=='mym2-empty':return {'mym2':script(language([]))}
    if policy=='mym2-no-language':return {'mym2':script(None)}
    if policy=='mym2-eng':return {'mym2':script(None,{'ENG ':language([0])})}
    return {policy:script(language([0]))}

def worker(args):
    start=time.perf_counter();base=args.output_dir/'identity.ttf';make_font(base);rows=[]
    if args.features:return feature_worker(args,base,start)
    if args.scripts:return script_worker(args,base,start)
    for index,(gs,gp) in enumerate(itertools.product(POLICIES,repeat=2)):
        if index//27!=args.batch:continue
        path=args.output_dir/(gs+'--'+gp+'.ttf')
        with TTFont(base,recalcTimestamp=False) as font:
            for kind,policy in (('GSUB',gs),('GPOS',gp)):
                if kind in font:del font[kind]
                if policy=='missing':continue
                font[kind]=newTable(kind);font[kind].table=layout(kind,[('ccmp' if kind=='GSUB' else 'dist',[0])],
                    [lookup(1,single({'u1000':'u1001'})) if kind=='GSUB' else adjust('u1000',11,17,31)],scripts(policy))
            font.save(path)
        values=[]
        for independent in (False,True):
            observed=batch_glyphs(args.reference,path,CASES,seed='1000',detailed=True,independent=independent,timeout=2)
            values.append(dict(independent=independent,rows=observed))
        rows.append(dict(gsub=gs,gpos=gp,font=identity(path),observations=values))
    result=dict(status='observed',seconds=time.perf_counter()-start,reference=identity(args.reference),
        source=identity(Path(__file__)),cases=CASES,fonts=rows)
    (args.output_dir/'result.json').write_text(json.dumps(result));print('observed',len(rows)*len(CASES)*2,'shapes');return 0

def feature_worker(args,base,start):
    from myanmar_plan_policy import SUBSTITUTION,POSITION
    rows=[];specs=[]
    for tag in (*SUBSTITUTION,'rkrf','haln'):
        specs.append(('GSUB','mymr',tag))
    for script_tag in ('mymr','DFLT','latn'):
        for tag in POSITION:specs.append(('GPOS',script_tag,tag))
    for tag in POSITION:specs.append(('GPOS-modern','latn',tag))
    for index,(kind,script_tag,tag) in enumerate(specs):
        if index%3!=args.batch:continue
        path=args.output_dir/(kind+'-'+script_tag+'-'+tag+'.ttf')
        with TTFont(base,recalcTimestamp=False) as f:
            del f['GSUB'];del f['GPOS']
            if kind=='GPOS-modern':
                f['GSUB']=newTable('GSUB');f['GSUB'].table=layout('GSUB',[],[],{'mym2':script(language([]))})
            table='GSUB' if kind=='GSUB' else 'GPOS';f[table]=newTable(table)
            f[table].table=layout(table,[(tag,[0])],
                [lookup(1,single({'u1000':'u1001'})) if table=='GSUB' else adjust('u1000',11,17,31)],
                {script_tag:script(language([0]))});f.save(path)
        observations=[]
        for independent in (False,True):observations.append(dict(independent=independent,
            rows=batch_glyphs(args.reference,path,CASES,seed='1000',detailed=True,independent=independent,timeout=2)))
        rows.append(dict(kind=kind,script=script_tag,feature=tag,font=identity(path),observations=observations))
    result=dict(status='observed',seconds=time.perf_counter()-start,reference=identity(args.reference),
        source=identity(Path(__file__)),cases=CASES,fonts=rows)
    (args.output_dir/'result.json').write_text(json.dumps(result));return 0

def script_worker(args,base,start):
    # Per-script markers make selection observable without unsorted feature tags.
    tags=('mym2','mymr','DFLT','latn');rows=[]
    specs=[('legacy-order',{'mymr':'default','DFLT':'default','latn':'default'},{}),
        ('legacy-default',{'DFLT':'default','latn':'default'},{}),
        ('legacy-eng',{'mym2':'eng','DFLT':'default','latn':'default'},{}),
        ('legacy-no-language',{'mymr':'none','DFLT':'default'},{}),
        ('modern-order',{'mym2':'default','DFLT':'default','latn':'default'},{}),
        ('modern-gpos-default',{'mym2':'empty'},{'DFLT':'default','latn':'default'}),
        ('modern-gpos-block',{'mym2':'empty'},{'mym2':'none','DFLT':'default','latn':'default'}),
        ('legacy-gpos-eng',{}, {'mym2':'eng','DFLT':'default','latn':'default'}),
        ('legacy-gpos-order',{}, {'mymr':'default','DFLT':'default','latn':'default'}),
        ('no-gdef',{'mym2':'default'},{}),('gpos-no-gdef',{}, {'mym2':'default'})]
    for gate in ('mym2','latn','DFLT','arab'):
        for policy in ('none','eng','empty'):
            specs.append(('gate-'+gate+'-'+policy,{}, {'mymr':'default',gate:policy}))
    for modern in (False,True):
        for policy in ('none','eng','empty'):
            specs.append(('fallback-'+str(int(modern))+'-'+policy,{'mym2':'empty'} if modern else {},{'latn':policy,'DFLT':'default'}))
    for name,gs,gp in specs:
        path=args.output_dir/(name+'.ttf')
        with TTFont(base,recalcTimestamp=False) as f:
            del f['GSUB'];del f['GPOS']
            if 'no-gdef' in name:del f['GDEF']
            for kind,selected in (('GSUB',gs),('GPOS',gp)):
                if not selected:continue
                records={}
                for tag,policy in selected.items():
                    indices=[tags.index(tag)] if tag in tags else [0]
                    records[tag]=script(language(indices) if policy=='default' else language([]) if policy=='empty' else None,
                        {'ENG ':language(indices)} if policy=='eng' else {})
                lookups=[lookup(1,single({'u1000':f'u{0x1001+i:04X}'})) if kind=='GSUB' else adjust('u1000',11,17,31*(i+1)) for i in range(4)]
                f[kind]=newTable(kind);f[kind].table=layout(kind,[('ccmp' if kind=='GSUB' else 'dist',[i]) for i in range(4)],lookups,records)
            f.save(path)
        observations=[dict(independent=mode,rows=batch_glyphs(args.reference,path,CASES,seed='1000',detailed=True,independent=mode,timeout=2)) for mode in (False,True)]
        rows.append(dict(name=name,font=identity(path),observations=observations))
    result=dict(status='observed',seconds=time.perf_counter()-start,reference=identity(args.reference),
        source=identity(Path(__file__)),cases=CASES,fonts=rows)
    (args.output_dir/'result.json').write_text(json.dumps(result));print('observed',len(rows)*len(CASES)*2,'shapes');return 0

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--reference',type=Path,required=True)
    p.add_argument('--output-dir',type=Path,required=True);p.add_argument('--batch',type=int,choices=(0,1,2),required=True)
    p.add_argument('--features',action='store_true');p.add_argument('--scripts',action='store_true')
    p.add_argument('--worker',action='store_true',help=argparse.SUPPRESS);args=p.parse_args()
    args.reference=args.reference.resolve();args.output_dir=args.output_dir.resolve()
    if args.worker:return worker(args)
    if args.output_dir.exists() and any(args.output_dir.iterdir()):p.error('Preserve existing evidence')
    args.output_dir.mkdir(parents=True,exist_ok=True);path=args.output_dir/'result.json';path.write_text('{"status":"running"}')
    sys.path.insert(0,str(Path(__file__).resolve().parents[1]));from verify_native_text import run_check,CheckFailure
    try:execution=run_check('myanmar-admission',[sys.executable,str(Path(__file__).resolve()),*sys.argv[1:],'--worker'],Path.cwd(),args.output_dir,5,threading.Event())
    except CheckFailure as e:execution=e.row
    result=json.loads(path.read_text());result['execution']=execution
    if execution['exit']:result['status']=execution['status']
    path.write_text(json.dumps(result));return execution['exit']

if __name__=='__main__':raise SystemExit(main())
