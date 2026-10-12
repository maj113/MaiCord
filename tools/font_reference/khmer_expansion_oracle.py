"""Observe native Khmer expansion inventories using an identity-mapped font.

No shape, position or raster parity claim: the comparison ignores glyph order
and removes the Windows-inserted dotted circle. Native source offsets and
required source-unit counts are checked separately. Offline FontTools only.
"""
import argparse
import json
from pathlib import Path
import struct
import sys
import threading
import time

from devanagari_plan_oracle import batch_glyphs
from tamil_cluster_oracle import identity
import subprocess,os
def native_stream(exe,cases,expand=False):
    payload=bytearray(b'MDVCL1\0\0'+struct.pack('<I',len(cases)))
    for s,g in cases:payload+=struct.pack('<I',len(s))+struct.pack('<'+'I'*len(s),*s)
    run=subprocess.run([str(exe)],input=payload,capture_output=True,timeout=2,
        env=dict(os.environ,MAICORD_KHMER_INPUT_STREAM='2' if expand else '1'),creationflags=subprocess.CREATE_NO_WINDOW)
    if run.returncode or run.stdout[:12]!=payload[:12]:raise RuntimeError('Invalid native expansion stream')
    return run.stdout[12:]


def make_font(path):
    from fontTools.fontBuilder import FontBuilder
    from fontTools.ttLib import newTable
    from fontTools.ttLib.tables._g_l_y_f import Glyph
    from feature_plan_oracle import layout, script, language
    from gsub_execution_oracle import lookup, single, obj
    scalars = list(range(0x1780, 0x1800)) + [0x25cc]
    order = ['.notdef'] + [f'u{c:04x}' for c in scalars]
    builder = FontBuilder(1000, isTTF=True)
    builder.setupGlyphOrder(order)
    builder.setupCharacterMap(dict(zip(scalars, order[1:])))
    builder.setupGlyf({g:Glyph() for g in order})
    builder.setupHorizontalMetrics({g:(600,0) for g in order})
    builder.setupHorizontalHeader(ascent=800, descent=-200)
    builder.setupNameTable(dict(familyName='MaiCord Khmer Expansion', styleName='Regular',
        uniqueFontIdentifier='MaiCordKhmerExpansion', fullName='MaiCord Khmer Expansion',
        psName='MaiCordKhmerExpansion'))
    builder.setupOS2(sTypoAscender=800, sTypoDescender=-200, usWinAscent=800, usWinDescent=200)
    builder.setupPost();builder.setupMaxp()
    builder.font['GSUB'] = newTable('GSUB')
    builder.font['GSUB'].table = layout('GSUB', [('ccmp',[0])], [lookup(1,single({'u1780':'u1780'}))],
        {'khmr':script(language([0]))})
    from gpos_execution_oracle import adjust
    builder.font['GPOS']=newTable('GPOS')
    builder.font['GPOS'].table=layout('GPOS',[('dist',[0])],[adjust('u1780',0,0,0)],{'khmr':script(language([0]))})
    # The installed Khmer shaper needs GDEF presence, even with no classes.
    builder.font['GDEF']=newTable('GDEF')
    builder.font['GDEF'].table=obj('GDEF',Version=0x10000,GlyphClassDef=None,AttachList=None,LigCaretList=None,MarkAttachClassDef=None)
    builder.font['head'].created = builder.font['head'].modified = 2082844800
    builder.save(path)
    return [None, *scalars]


def worker(args):
    started = time.perf_counter()
    font = args.output_dir/'identity.ttf'
    cases = [(prefix+(c,), 'single-scalar-or-base-pair') for prefix in ((),(0x1780,),(0x17a5,),(0x17e0,))
             for c in range(0x1780, 0x1800)]
    if args.case_file:cases=[(tuple(s),'explicit-cluster') for s in json.loads(args.case_file.read_text())]
    if args.observations:
        saved=json.loads(args.observations.read_text(encoding='utf-8'))
        font=Path(saved['inputs'][0]['path'])
        if saved['inputs']!=[identity(font),identity(args.reference)] or [r['scalars'] for r in saved['rows']]!=[list(s) for s,_ in cases]:
            raise ValueError('Font, observer or corpus observations changed')
        glyph_to_scalar=[None,*range(0x1780,0x1800),0x25cc]
        observations=[r['result'] for r in saved['rows']]
    else:
        glyph_to_scalar = make_font(font)
        observations = batch_glyphs(args.reference, font, [s for s,_ in cases], seed='1780', detailed=True, timeout=2)
    inputs = [identity(font),identity(args.reference)]
    (args.output_dir/'observations.json').write_text(json.dumps(dict(inputs=inputs,
        rows=[dict(scalars=s,result=r) for (s,_),r in zip(cases,observations)])), encoding='utf-8')
    data = native_stream(args.native, cases, expand=True)
    at = 0;rows = []
    for (scalars,_), reference in zip(cases,observations):
        count,units = struct.unpack_from('<II', data, at);at += 8
        native = [list(struct.unpack_from('<II',data,at+i*8)) for i in range(count)];at += count*8
        inventory = [glyph_to_scalar[g] for g in reference['glyphs'] if glyph_to_scalar[g]!=0x25cc]
        # Check source consistency separately from the observed glyph inventory.
        # Each original survives at its UTF-16 offset, with an optional leading
        # 17C1 from that same source. Individual Windows glyph origins are not
        # exposed by its cluster map, so this is not an origin-parity claim.
        sources_ok=units==len(scalars);cursor=0
        for source,c in enumerate(scalars):
            pieces=[]
            while cursor<len(native) and native[cursor][1]==source:pieces.append(native[cursor][0]);cursor+=1
            sources_ok &= pieces==[c] or (c in (0x17be,0x17bf,0x17c0,0x17c4,0x17c5) and pieces==[0x17c1,c])
        exact=sources_ok and cursor==len(native) and sorted(v[0] for v in native)==sorted(inventory)
        rows.append(dict(scalars=scalars,native=native,reference_inventory=inventory,exact=exact))
    if at!=len(data):
        raise RuntimeError('Trailing native expansion data')
    result = dict(status='passed' if all(r['exact'] for r in rows) else 'failed', cases=len(rows),
        exact=sum(r['exact'] for r in rows), seconds=time.perf_counter()-started, scope=__doc__,
        inputs=inputs,native=identity(args.native),rows=rows)
    (args.output_dir/'result.json').write_text(json.dumps(result), encoding='utf-8')
    print(f'{result["exact"]}/{len(rows)} expansion inventories in {result["seconds"]:.2f}s')
    return 0 if result['status']=='passed' else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('native','reference','output-dir'):
        parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--worker',action='store_true',help=argparse.SUPPRESS)
    parser.add_argument('--observations',type=Path)
    parser.add_argument('--case-file',type=Path)
    args = parser.parse_args()
    for name in ('native','reference','output_dir'):
        setattr(args,name,getattr(args,name).resolve())
    if args.worker:
        return worker(args)
    args.output_dir.mkdir(parents=True,exist_ok=True)
    if args.observations and args.observations.resolve()==(args.output_dir/'observations.json').resolve():parser.error('Keep saved observations separate')
    path = args.output_dir/'result.json';path.write_text('{"status":"running"}',encoding='utf-8')
    sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
    from verify_native_text import run_check,CheckFailure
    try:
        execution=run_check('khmer-expansion',[sys.executable,str(Path(__file__).resolve()),*sys.argv[1:],'--worker'],
            Path.cwd(),args.output_dir,5,threading.Event())
    except CheckFailure as error:
        execution=error.row
    result=json.loads(path.read_text(encoding='utf-8'));result['execution']=execution
    if execution['exit']:result['status']=execution['status']
    path.write_text(json.dumps(result),encoding='utf-8')
    print({k:result[k] for k in ('status','cases','exact') if k in result})
    return execution['exit']


if __name__=='__main__':
    raise SystemExit(main())
