"""Observe native Sinhala expansion inventories using an identity-mapped font.

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
from verify_sinhala_input import native_stream


def make_font(path):
    from fontTools.fontBuilder import FontBuilder
    from fontTools.ttLib import newTable
    from fontTools.ttLib.tables._g_l_y_f import Glyph
    from feature_plan_oracle import layout, script, language
    from gsub_execution_oracle import lookup, single
    scalars = list(range(0xd80, 0xe00)) + [0x25cc]
    order = ['.notdef'] + [f'u{c:04x}' for c in scalars]
    builder = FontBuilder(1000, isTTF=True)
    builder.setupGlyphOrder(order)
    builder.setupCharacterMap(dict(zip(scalars, order[1:])))
    builder.setupGlyf({g:Glyph() for g in order})
    builder.setupHorizontalMetrics({g:(600,0) for g in order})
    builder.setupHorizontalHeader(ascent=800, descent=-200)
    builder.setupNameTable(dict(familyName='MaiCord Sinhala Expansion', styleName='Regular',
        uniqueFontIdentifier='MaiCordSinhalaExpansion', fullName='MaiCord Sinhala Expansion',
        psName='MaiCordSinhalaExpansion'))
    builder.setupOS2(sTypoAscender=800, sTypoDescender=-200, usWinAscent=800, usWinDescent=200)
    builder.setupPost();builder.setupMaxp()
    builder.font['GSUB'] = newTable('GSUB')
    builder.font['GSUB'].table = layout('GSUB', [('ccmp',[0])], [lookup(1,single({'u0d9a':'u0d9a'}))],
        {'sinh':script(language([0]))})
    builder.font['head'].created = builder.font['head'].modified = 2082844800
    builder.save(path)
    return [None, *scalars]


def worker(args):
    started = time.perf_counter()
    font = args.output_dir/'identity.ttf'
    cases = [(prefix+(c,), 'single-scalar-or-base-pair') for prefix in ((),(0xd9a,),(0xd85,),(0xd91,))
             for c in range(0xd80, 0xe00)]
    if args.observations:
        saved=json.loads(args.observations.read_text(encoding='utf-8'))
        font=Path(saved['inputs'][0]['path'])
        if saved['inputs']!=[identity(font),identity(args.reference)] or [r['scalars'] for r in saved['rows']]!=[list(s) for s,_ in cases]:
            raise ValueError('Font, observer or corpus observations changed')
        glyph_to_scalar=[None,*range(0xd80,0xe00),0x25cc]
        observations=[r['result'] for r in saved['rows']]
    else:
        glyph_to_scalar = make_font(font)
        observations = batch_glyphs(args.reference, font, [s for s,_ in cases], seed='d9a', detailed=True, timeout=2)
    inputs = [identity(font),identity(args.reference)]
    (args.output_dir/'observations.json').write_text(json.dumps(dict(inputs=inputs,
        rows=[dict(scalars=s,result=r) for (s,_),r in zip(cases,observations)])), encoding='utf-8')
    data = native_stream(args.native, cases, expand=True)
    at = 0;rows = []
    for (scalars,_), reference in zip(cases,observations):
        count,units = struct.unpack_from('<II', data, at);at += 8
        native = [list(struct.unpack_from('<II',data,at+i*8)) for i in range(count)];at += count*8
        inventory = [glyph_to_scalar[g] for g in reference['glyphs'] if glyph_to_scalar[g]!=0x25cc]
        # Every source unit has one nominal component or two split components;
        # no source offset is invented, lost, or moved to the neighboring base.
        origins = [i for i,c in enumerate(scalars) for _ in range(2 if c in (0xdda,0xddc,0xddd,0xdde) else 1)]
        exact = units==len(scalars) and [v[1] for v in native]==origins and sorted(v[0] for v in native)==sorted(inventory)
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
        execution=run_check('sinhala-expansion',[sys.executable,str(Path(__file__).resolve()),*sys.argv[1:],'--worker'],
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
