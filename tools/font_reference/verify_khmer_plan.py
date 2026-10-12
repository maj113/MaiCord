"""Replay saved Windows Khmer single-lookup fixtures through native plans.

A tiny offline QA interpreter starts from saved Windows control glyph order.
CFAR eligibility is supplied for these explicit contexts, not computed natively.
GSUB comparisons cover glyphs only; GPOS comparisons cover placement bits too.
This verifies scheduling, not the native executor, syllable rules or pixels.
"""
import argparse,json,struct,sys,time,threading
from pathlib import Path
from fontTools.ttLib import TTFont
from gsub_execution_oracle import identity
from verify_sinhala_plan import plans

def bits(value):return struct.unpack('<I',struct.pack('<f',value))[0]
def scalar(value):return struct.unpack('<f',struct.pack('<I',value))[0]
def read(path):
    saved=json.loads(path.read_text(encoding='utf-8'))
    if saved['status']!='observed' or saved['execution']['exit']:raise RuntimeError('Incomplete observation set')
    for case in saved['checks']:
        if identity(Path(case['font']['path']))!=case['font']:raise RuntimeError('Fixture identity changed')
    return saved

def worker(args):
    start=time.perf_counter();checks=[]
    control=next(c for c in read(args.control)['checks'] if c['name']=='control')
    baseline={(o['context'],o['mode']):o for o in control['observations']}
    for path in args.observations:
        for case in read(path)['checks']:
            font_path=Path(case['font']['path']);rows=plans(args.native,font_path)
            with TTFont(font_path) as font:
                for observed in case['observations']:
                    mode=int(observed['mode']!='default');context=observed['context']
                    base=baseline[context,observed['mode']]
                    glyphs=list(base['glyph_names'])
                    placements=[[scalar(v) for v in line] for line in base['result']['placement_bits']]
                    positional=case['name'].startswith(('GPOS-','shared-pos-','required-pos-'))
                    for row in rows:
                        if row['independent']!=mode:continue
                        table=font['GPOS' if row['kind'] else 'GSUB'].table
                        for index,mask,stage in row['steps']:
                            if mask not in (0,1) or (mask and (row['kind'] or stage!=1)):raise RuntimeError('Invalid eligibility mask')
                            if mask and context!='after-ro':continue
                            lookup=table.LookupList.Lookup[index]
                            if lookup.LookupType!=1 or lookup.LookupFlag:raise RuntimeError('Outside single-lookup interpreter scope')
                            for at,glyph in enumerate(glyphs):
                                for sub in lookup.SubTable:
                                    if not row['kind'] and glyph in sub.mapping:
                                        glyphs[at]=sub.mapping[glyph];break
                                    if row['kind'] and glyph in sub.Coverage.glyphs:
                                        if sub.Format!=1:raise RuntimeError('Outside SinglePos format one')
                                        placements[at][0]+=getattr(sub.Value,'XAdvance',0);break
                    projected=[[bits(v) for v in line] for line in placements]
                    exact=glyphs==observed['glyph_names'] and (not positional or projected==observed['result']['placement_bits'])
                    checks.append(dict(name=case['name'],context=context,mode=mode,exact=exact,
                        projected_glyphs=glyphs,reference=observed['glyph_names'],
                        placement_exact=(projected==observed['result']['placement_bits']) if positional else None))
    result=dict(status='passed' if checks and all(c['exact'] for c in checks) else 'failed',
        seconds=time.perf_counter()-start,cases=len(checks),native=identity(args.native),scope=__doc__,
        sources=[identity(Path(__file__))],control=identity(args.control),observations=[identity(p) for p in args.observations],checks=checks)
    (args.output_dir/'result.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(sum(c['exact'] for c in checks),'/',len(checks),'checks',round(result['seconds'],2),'seconds')
    return 0 if result['status']=='passed' else 1

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native',required=True,type=Path);parser.add_argument('--control',required=True,type=Path)
    parser.add_argument('--observations',required=True,type=Path,action='append');parser.add_argument('--output-dir',required=True,type=Path)
    parser.add_argument('--worker',action='store_true',help=argparse.SUPPRESS);args=parser.parse_args()
    if args.worker:return worker(args)
    args.output_dir.mkdir(parents=True,exist_ok=True);path=args.output_dir/'result.json'
    if path.resolve() in [args.control.resolve(),*(p.resolve() for p in args.observations)]:parser.error('Preserve saved observations')
    path.write_text('{"status":"running"}',encoding='utf-8')
    sys.path.insert(0,str(Path(__file__).resolve().parents[1]));from verify_native_text import run_check,CheckFailure
    try:execution=run_check('khmer-plan-comparison',[sys.executable,str(Path(__file__).resolve()),*sys.argv[1:],'--worker'],Path.cwd(),args.output_dir,5,threading.Event())
    except CheckFailure as error:execution=error.row
    result=json.loads(path.read_text(encoding='utf-8'));result['execution']=execution
    if execution['exit']:result['status']=execution['status']
    path.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');return execution['exit']
if __name__=='__main__':raise SystemExit(main())
