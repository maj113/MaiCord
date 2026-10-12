"""Bounded authored Tibetan font-admission observations; offline QA only."""
import argparse,itertools,json,sys,threading,time
from pathlib import Path
from fontTools.ttLib import TTFont,newTable
from tibetan_input_samples import make_font
from feature_plan_oracle import layout,script,language
from gsub_execution_oracle import lookup,single,identity
from gpos_execution_oracle import adjust
from devanagari_plan_oracle import batch_glyphs

POLICIES=('missing','empty-scripts','tibt','tibt-empty','tibt-no-language','tibt-eng','DFLT','latn','tib2')
CASES=[[0xf40,0xf73],[0xf73],[0xf40,0xf3f],[0xf40,0xf90,0xf72],
       [0xf40,0x200d],[0x34f,0x34f],[0x200d,0x200c],[0xf40,0xfe00,0xf73],
       [0xf40,0xf73,0xf40],[0xf72,0xf74],[0xf40,0x2060],[0xf7f],
       [0xf40,0xf76],[0xf40,0xf78],[0xf40,0xf81],[0xf40,0xf90,0xf40,0xf3f]]

def scripts(policy):
    if policy=='empty-scripts':return {}
    if policy=='tibt-empty':return {'tibt':script(language([]))}
    if policy=='tibt-no-language':return {'tibt':script(None)}
    if policy=='tibt-eng':return {'tibt':script(None,{'ENG ':language([0])})}
    return {policy:script(language([0]))}

def worker(args):
    start=time.perf_counter();base=args.output_dir/'identity.ttf';make_font(base);rows=[]
    for index,(gs,gp) in enumerate(itertools.product(POLICIES,repeat=2)):
        if index//27!=args.batch:continue
        path=args.output_dir/(gs+'--'+gp+'.ttf')
        with TTFont(base,recalcTimestamp=False) as font:
            for kind,policy in (('GSUB',gs),('GPOS',gp)):
                if kind in font:del font[kind]
                if policy=='missing':continue
                font[kind]=newTable(kind);font[kind].table=layout(kind,[('ccmp' if kind=='GSUB' else 'dist',[0])],
                    [lookup(1,single({'u0f40':'u0f41'})) if kind=='GSUB' else adjust('u0f40',11,17,31)],scripts(policy))
            font.save(path)
        values=[]
        for independent in (False,True):
            observed=batch_glyphs(args.reference,path,CASES,seed='0f40',detailed=True,independent=independent,timeout=2)
            values.append(dict(independent=independent,rows=observed))
        rows.append(dict(gsub=gs,gpos=gp,font=identity(path),observations=values))
    result=dict(status='observed',seconds=time.perf_counter()-start,reference=identity(args.reference),
        source=identity(Path(__file__)),cases=CASES,fonts=rows)
    (args.output_dir/'result.json').write_text(json.dumps(result));print('observed',len(rows)*len(CASES)*2,'shapes');return 0

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--reference',type=Path,required=True)
    p.add_argument('--output-dir',type=Path,required=True);p.add_argument('--batch',type=int,choices=(0,1,2),required=True)
    p.add_argument('--worker',action='store_true',help=argparse.SUPPRESS);args=p.parse_args()
    args.reference=args.reference.resolve();args.output_dir=args.output_dir.resolve()
    if args.worker:return worker(args)
    if args.output_dir.exists() and any(args.output_dir.iterdir()):p.error('Preserve existing evidence')
    args.output_dir.mkdir(parents=True,exist_ok=True);path=args.output_dir/'result.json';path.write_text('{"status":"running"}')
    sys.path.insert(0,str(Path(__file__).resolve().parents[1]));from verify_native_text import run_check,CheckFailure
    try:execution=run_check('tibetan-admission',[sys.executable,str(Path(__file__).resolve()),*sys.argv[1:],'--worker'],Path.cwd(),args.output_dir,5,threading.Event())
    except CheckFailure as e:execution=e.row
    result=json.loads(path.read_text());result['execution']=execution
    if execution['exit']:result['status']=execution['status']
    path.write_text(json.dumps(result));return execution['exit']

if __name__=='__main__':raise SystemExit(main())
