"""Compare native Tibetan expansion with Windows identity-font inventories.

Dotted circles and glyph order are separate shaping work. Per-source expansion
uses isolated Windows scalar observations; multi-scalar checks compare inventory
only, explicitly discarding inserted circles and later reordering.
"""
import argparse,itertools,json,os,struct,subprocess,sys,threading,time
from pathlib import Path
from fontTools.ttLib import TTFont
from devanagari_plan_oracle import batch_glyphs,identity

def worker(args):
    start=time.perf_counter();scalars=list(range(0xf00,0x1000))+[0x1f600]
    with TTFont(args.font) as font:
        order=font.getGlyphOrder();reverse={g:c for c,g in font.getBestCmap().items()}
    cases=[[c] for c in scalars]+[[0xf40,c] for c in scalars]
    vowels=(0xf73,0xf75,0xf76,0xf77,0xf78,0xf79,0xf81)
    cases += [[*prefix,*pair] for prefix in ((),(0xf40,),(0x1f600,)) for pair in itertools.product(vowels,repeat=2)]
    observed=batch_glyphs(args.reference,args.font,cases,seed='0f40',detailed=True,timeout=2)
    def inventory(row):return [reverse[order[g]] for g in row['glyphs'] if reverse[order[g]]!=0x25cc]
    single={c:inventory(row) for c,row in zip(scalars,observed)}
    payload=bytearray(b'MDVCL1\0\0'+struct.pack('<I',len(cases)))
    for s in cases:payload+=struct.pack('<I',len(s))+struct.pack('<'+'I'*len(s),*s)
    run=subprocess.run([str(args.native)],input=payload,capture_output=True,timeout=2,
        env=dict(os.environ,MAICORD_TIBETAN_INPUT_STREAM='2'),creationflags=subprocess.CREATE_NO_WINDOW)
    if run.returncode or run.stdout[:12]!=payload[:12]:raise RuntimeError('Invalid native expansion header')
    cursor=12;checks=[]
    for s,reference in zip(cases,observed):
        count,units=struct.unpack_from('<II',run.stdout,cursor);cursor+=8
        values=[list(struct.unpack_from('<II',run.stdout,cursor+i*8)) for i in range(count)];cursor+=count*8
        expected=[];source=0
        for scalar in s:
            expected += [[piece,source] for piece in single[scalar]];source+=1+(scalar>65535)
        whole=inventory(reference)
        exact=values==expected and units==source and sorted(v[0] for v in values)==sorted(whole)
        checks.append(dict(scalars=s,exact=exact,native=values,expected=expected,windows_inventory=whole,windows=reference))
    if cursor!=len(run.stdout):raise RuntimeError('Trailing native expansion data')
    result=dict(status='passed' if all(c['exact'] for c in checks) else 'failed',cases=len(checks),
        exact=sum(c['exact'] for c in checks),seconds=time.perf_counter()-start,scope=__doc__,
        font=identity(args.font),reference=identity(args.reference),native=identity(args.native),source=identity(Path(__file__)),checks=checks)
    (args.output_dir/'result.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(result['exact'],'/',result['cases'],'expansion inventories');return int(result['status']!='passed')

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for key in ('font','reference','native','output-dir'):p.add_argument('--'+key,type=Path,required=True)
    p.add_argument('--worker',action='store_true',help=argparse.SUPPRESS);args=p.parse_args()
    if args.worker:return worker(args)
    args.output_dir.mkdir(parents=True,exist_ok=True);path=args.output_dir/'result.json'
    if path.exists():p.error('Preserve existing evidence')
    path.write_text('{"status":"running"}',encoding='utf-8')
    sys.path.insert(0,str(Path(__file__).resolve().parents[1]));from verify_native_text import run_check,CheckFailure
    try:r=run_check('tibetan-expansion',[sys.executable,str(Path(__file__).resolve()),*sys.argv[1:],'--worker'],Path.cwd(),args.output_dir,5,threading.Event())
    except CheckFailure as error:r=error.row
    result=json.loads(path.read_text());result['execution']=r
    if r['exit']:result['status']=r['status']
    path.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');return r['exit']
if __name__=='__main__':raise SystemExit(main())
