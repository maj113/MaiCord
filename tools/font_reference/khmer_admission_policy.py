"""Bounded Khmer GSUB/GPOS admission observations; offline QA only."""
import argparse,itertools,json,subprocess,sys,threading,time
from pathlib import Path
from fontTools.ttLib import TTFont,newTable
from khmer_plan_policy import CMAP,CONTEXTS
from feature_plan_oracle import language,script,layout
from gsub_execution_oracle import make_font,lookup,single,marker,ORDER,identity
from gpos_execution_oracle import adjust

POLICIES=('missing','empty-scripts','DFLT','latn','khmr','khmr-empty','khmr-eng','khm2')
def scripts(policy):
    if policy=='empty-scripts':return {}
    if policy=='khmr-empty':return {'khmr':script(language([]))}
    if policy=='khmr-eng':return {'khmr':script(None,{'ENG ':language([0])})}
    return {policy:script(language([0]))}

def worker(args):
    start=time.perf_counter();rows=[]
    for index,(gsub,gpos) in enumerate(itertools.product(POLICIES,repeat=2)):
        if index//32!=args.batch:continue
        path=args.output_dir/(gsub+'--'+gpos+'.ttf')
        make_font(path,dict(name='khmer-admission',lookups=[lookup(1,single({'A':'A'}))]))
        with TTFont(path,recalcTimestamp=False) as font:
            for cmap in font['cmap'].tables:
                if cmap.isUnicode():cmap.cmap.update(CMAP)
            for kind,policy in [('GSUB',gsub),('GPOS',gpos)]:
                if kind in font:del font[kind]
                if policy=='missing':continue
                font[kind]=newTable(kind)
                font[kind].table=layout(kind,[('ccmp' if kind=='GSUB' else 'dist',[0])],
                    [marker() if kind=='GSUB' else adjust('A',0,0,31)],scripts(policy))
            font.save(path)
        row=dict(gsub=gsub,gpos=gpos,font=identity(path),observations=[]);rows.append(row)
        for context in ('base','above-split'):
            for mode in ('default','liga=0,clig=0,kern=0'):
                run=subprocess.run([str(args.reference),str(path),CONTEXTS[context],mode,'1148846080','0','0','en-US'],
                    capture_output=True,text=True,timeout=2,creationflags=subprocess.CREATE_NO_WINDOW)
                if run.returncode:raise RuntimeError(run.stdout+run.stderr)
                value=json.loads(run.stdout)
                if value['hresult'] or value['placement_hresult']:raise RuntimeError(value)
                row['observations'].append(dict(context=context,mode=mode,glyph_names=[ORDER[g] for g in value['glyphs']],result=value))
    result=dict(status='observed',seconds=time.perf_counter()-start,reference=identity(args.reference),source=identity(Path(__file__)),checks=rows)
    (args.output_dir/'result.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(len(rows)*4,'observations',round(result['seconds'],2),'seconds');return 0

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--reference',required=True,type=Path)
    p.add_argument('--output-dir',required=True,type=Path);p.add_argument('--batch',required=True,type=int,choices=(0,1))
    p.add_argument('--worker',action='store_true',help=argparse.SUPPRESS);args=p.parse_args()
    if args.worker:return worker(args)
    args.output_dir.mkdir(parents=True,exist_ok=True);path=args.output_dir/'result.json';path.write_text('{"status":"running"}',encoding='utf-8')
    sys.path.insert(0,str(Path(__file__).resolve().parents[1]));from verify_native_text import run_check,CheckFailure
    try:execution=run_check('khmer-admission',[sys.executable,str(Path(__file__).resolve()),*sys.argv[1:],'--worker'],Path.cwd(),args.output_dir,5,threading.Event())
    except CheckFailure as error:execution=error.row
    result=json.loads(path.read_text(encoding='utf-8'));result['execution']=execution
    if execution['exit']:result['status']=execution['status']
    path.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');return execution['exit']
if __name__=='__main__':raise SystemExit(main())
