"""Compare native character preparation with identity-font Windows glyph order.

Offline projection resolves this fixture's nominal cmap, consumes one adjacent
original selector and applies its measured zero-glyph convention. No native
GSUB/GPOS, font admission, placement or pixel acceptance is claimed.
"""
import argparse,json,os,struct,subprocess,sys,threading,time
from pathlib import Path
from fontTools.ttLib import TTFont
from devanagari_plan_oracle import batch_glyphs
from gsub_execution_oracle import identity

def selector(c):return 0xfe00<=c<=0xfe0f or 0xe0100<=c<=0xe01ef
def worker(args):
    start=time.perf_counter();cases=json.loads(args.case_file.read_text())
    if not cases or any(not s or len(s)>256 or sum(1+(c>65535) for c in s)>256 for s in cases):raise ValueError('Observer capacity')
    inputs=[identity(args.font),identity(args.reference),identity(args.case_file)]
    with TTFont(args.font) as font:
        # Precondition: the font advertises the modern Myanmar script (mym2).
        for tag in ('GSUB','GPOS'):
            if 'mym2' not in [r.ScriptTag for r in font[tag].table.ScriptList.ScriptRecord]:raise ValueError('Missing Myanmar script')
        cmap={c:font.getGlyphID(g) for c,g in font.getBestCmap().items()}
    if args.observations:
        saved=json.loads(args.observations.read_text())
        if saved['inputs']!=inputs:raise ValueError('Changed reference inputs')
        observed=saved['rows']
    else:
        observed=[]
        for at in range(0,len(cases),128):
            rows=batch_glyphs(args.reference,args.font,cases[at:at+128],seed='1000',detailed=True,timeout=2)
            observed.extend(rows)
        (args.output_dir/'observations.json').write_text(json.dumps(dict(inputs=inputs,rows=observed)))
    payload=bytearray(b'MDVCL1\0\0'+struct.pack('<I',len(cases)))
    for s in cases:payload+=struct.pack('<I',len(s))+struct.pack('<'+'I'*len(s),*s)
    run=subprocess.run([str(args.native)],input=payload,capture_output=True,timeout=2,
        env=dict(os.environ,MAICORD_MYANMAR_INPUT_STREAM='2'),creationflags=subprocess.CREATE_NO_WINDOW)
    if run.returncode or run.stdout[:12]!=b'MDVPR1\0\0'+payload[8:12]:raise RuntimeError('Invalid native preparation stream')
    cursor=12;checks=[];zero={cmap.get(c,0) for c in (0x200b,0x200c,0x200d)}
    for s,ref in zip(cases,observed):
        count,units=struct.unpack_from('<II',run.stdout,cursor);cursor+=8
        prepared=[struct.unpack_from('<II',run.stdout,cursor+i*8) for i in range(count)];cursor+=count*8
        source={};offset=0
        for c in s:source[offset]=c;offset+=1+(c>65535)
        if units!=offset or any(at not in source or (c not in (0x25cc,0) and source[at]!=c) for c,at in prepared):raise RuntimeError('Invalid original source offsets')
        glyphs=[];at=0
        while at<count:
            c,src=prepared[at];glyph=cmap.get(c,0);glyphs.append(cmap[0x20] if glyph in zero else glyph)
            if at+1<count and not selector(c) and selector(prepared[at+1][0]) and prepared[at+1][1]==src+1+(c>65535):at+=1
            at+=1
        checks.append(dict(scalars=s,exact=glyphs==ref['glyphs'],native=glyphs,reference=ref['glyphs'],prepared=prepared))
    if cursor!=len(run.stdout) or len(observed)!=len(cases):raise RuntimeError('Trailing/truncated records')
    result=dict(status='passed' if all(r['exact'] for r in checks) else 'failed',cases=len(checks),exact=sum(r['exact'] for r in checks),
                seconds=time.perf_counter()-start,inputs=inputs,native=identity(args.native),scope=__doc__,rows=checks)
    (args.output_dir/'result.json').write_text(json.dumps(result));print(result['exact'],'/',len(checks),round(result['seconds'],3),'seconds')
    return int(result['status']!='passed')
def main():
    p=argparse.ArgumentParser(description=__doc__)
    for key in ('font','reference','case-file','native','output-dir'):p.add_argument('--'+key,required=True,type=Path)
    p.add_argument('--observations',type=Path);p.add_argument('--worker',action='store_true',help=argparse.SUPPRESS);args=p.parse_args()
    if args.worker:return worker(args)
    args.output_dir.mkdir(parents=True,exist_ok=True);path=args.output_dir/'result.json'
    if path.exists():p.error('Preserve existing evidence')
    path.write_text('{"status":"running"}')
    sys.path.insert(0,str(Path(__file__).resolve().parents[1]));from verify_native_text import run_check,CheckFailure
    try:r=run_check('myanmar-reorder',[sys.executable,str(Path(__file__).resolve()),*sys.argv[1:],'--worker'],Path.cwd(),args.output_dir,5,threading.Event())
    except CheckFailure as e:r=e.row
    result=json.loads(path.read_text());result['execution']=r
    if r['exit']:result['status']=r['status']
    path.write_text(json.dumps(result));return r['exit']
if __name__=='__main__':raise SystemExit(main())
