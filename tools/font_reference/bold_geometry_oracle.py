"""First-party synthetic-bold probes. DirectWrite and FontTools are QA-only.

Capture the measured outline transformation before selecting a native algorithm.
The native comparison is expected to differ for bold until it is implemented.
"""
import argparse, hashlib, json, struct, subprocess
from pathlib import Path
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen


def identity(path):
    data=path.read_bytes()
    return dict(path=str(path.resolve()),bytes=len(data),sha256=hashlib.sha256(data).hexdigest())


def shapes():
    result={'.notdef': [], 'rectangle': [[(100,0),(100,700),(500,700),(500,0)]],
            'triangle': [[(100,0),(300,700),(500,0)]],
            'diamond': [[(300,0),(100,350),(300,700),(500,350)]],
            'concave': [[(100,0),(100,700),(500,700),(500,500),(300,500),(300,200),(500,200),(500,0)]],
            'hole': [[(100,0),(100,700),(500,700),(500,0)],[(200,100),(400,100),(400,600),(200,600)]],
            'thin': [[(100,0),(100,700),(110,700),(110,0)]],
            'point': [[(100,200)]], 'line': [[(100,0),(100,700)]],
            'duplicate': [[(100,0),(100,0),(100,700),(500,700),(500,0)]],
            'collinear': [[(100,0),(100,350),(100,700),(500,700),(500,0)]],
            'bowtie': [[(100,0),(500,700),(100,700),(500,0)]],
            'above': [[(100,1200),(100,1900),(500,1900),(500,1200)]],
            'below': [[(100,-1200),(100,-500),(500,-500),(500,-1200)]]}
    for name,contours in list(result.items()):
        if not contours: continue
        result[name+'_reverse']=[list(reversed(c)) for c in contours]
        result[name+'_translated']=[[(x-613,y-257) for x,y in c] for c in contours]
    for dx,dy in [(1,700),(10,700),(100,700),(200,700),(300,400),(400,300),(700,200),(700,100),(700,10),(700,1),(700,0)]:
        result[f'angle_{dx}_{dy}']=[[(100,0),(100+dx,dy),(100+dx+200,dy),(300,0)]]
    for name in ['rectangle','triangle','diamond','concave','bowtie']:
        contour=result[name][0]
        for first in range(1,len(contour)):
            result[f'{name}_start_{first}']=[contour[first:]+contour[:first]]
        for scale in [0.1,0.5,2,10]:
            result[f'{name}_scale_{scale}']=[[(round(x*scale),round(y*scale)) for x,y in contour]]
    return result


def make_font(path,units,extra_shapes=None):
    contours=shapes();glyphs={}
    if extra_shapes:contours.update(extra_shapes)
    for name,paths in contours.items():
        pen=TTGlyphPen(None)
        for points in paths:
            pen.moveTo(points[0])
            for p in points[1:]:pen.lineTo(p)
            pen.closePath()
        glyphs[name]=pen.glyph()
    pen=TTGlyphPen(None);pen.moveTo((100,0));pen.qCurveTo((0,350),(100,700));pen.lineTo((500,700));pen.qCurveTo((600,350),(500,0));pen.closePath()
    glyphs['quadratic']=pen.glyph()
    for name in ['rectangle','quadratic','hole']:
        for suffix in ['hzero','vzero','bothzero']:glyphs[name+'_'+suffix]=glyphs[name]
    order=list(glyphs);fb=FontBuilder(units,isTTF=True);fb.setupGlyphOrder(order)
    fb.setupCharacterMap({0xE000+i:n for i,n in enumerate(order[1:],1)})
    fb.setupGlyf(glyphs)
    fb.setupHorizontalMetrics({name:(0 if name.endswith(('hzero','bothzero')) else 800,g.xMin if hasattr(g,'xMin') else 0) for name,g in glyphs.items()})
    fb.setupHorizontalHeader(ascent=800,descent=-200)
    fb.setupVerticalMetrics({name:(0 if name.endswith(('vzero','bothzero')) else 1000,100) for name in glyphs})
    fb.setupVerticalHeader(ascent=800,descent=-200)
    fb.setupOS2(sTypoAscender=800,sTypoDescender=-200,usWinAscent=800,usWinDescent=300)
    fb.setupNameTable({'familyName':'MaiCord Bold Geometry QA','styleName':'Regular','uniqueFontIdentifier':f'MaiCord Bold Geometry QA {units}','fullName':'MaiCord Bold Geometry QA','psName':'MaiCordBoldGeometryQA'})
    fb.setupPost();fb.setupMaxp();fb.font['head'].created=fb.font['head'].modified=3406620153
    fb.save(path)
    return order


def decode(row,units):
    def f(v):return struct.unpack('<f',struct.pack('<I',v))[0]
    commands=[]
    for at in range(8,len(row),7):
        bits=row[at:at+7];kind=bits[6]
        coords=[f(v)*units for v in bits[:6]]
        for i in [1,3,5]:coords[i]=-coords[i]
        commands.append(dict(kind=kind,design_coordinates=coords))
    return commands


def emit_metrics(data,header):
    records=set()
    for case in data['cases']:
        for mode in range(4):
            for name,base in case['metrics']['0'].items():
                records.add((case['units'],mode,*base,*case['metrics'][str(mode)][name]))
    blob=bytearray(b'MCSIMT1\0');blob.extend(struct.pack('<I',len(records)))
    for record in sorted(records):blob.extend(struct.pack('<16I',*(v&0xffffffff for v in record)))
    header.parent.mkdir(parents=True,exist_ok=True)
    header.write_text('// First-party design metrics and offline Windows simulation observations.\n'
        'static constexpr u8 simulation_metrics_fixture[]{\n'+''.join('    '+','.join(str(v) for v in blob[i:i+24])+',\n' for i in range(0,len(blob),24))+'};\n')
    print(f'{len(records)} unique metric cases; {len(blob)} fixture bytes')


def analyse_geometry(data):
    def f32(v):return struct.unpack('<f',struct.pack('<f',v))[0]
    def half(v):return int(v+.5) if v>=0 else -int(-v+.5)
    checks=bad=rectangle_bad=0
    for case in data['cases']:
        units=case['units'];scale=f32(1/units);strength=(units+49)//50
        plain=case['outlines']['0']['rectangle'];bold=case['outlines']['1']['rectangle']
        assert [c['kind'] for c in plain]==[c['kind'] for c in bold]
        shifts=[tuple(round((b['design_coordinates'][i]-a['design_coordinates'][i])*64)/64 for i in range(2)) for a,b in zip(plain,bold) if a['kind']!=5]
        rectangle_bad+=shifts!=[(0,0),(0,strength-1),(strength,strength-1),(strength,0)]
        for name,bold in case['outlines']['1'].items():
            combined=case['outlines']['3'][name]
            assert [c['kind'] for c in bold]==[c['kind'] for c in combined]
            if any(c['kind']==3 for c in bold):continue
            for b,c in zip(bold,combined):
                if b['kind']==5:continue
                x,y=[round(v*64)/64 for v in b['design_coordinates'][:2]]
                expected_x=f32((x+half(y*87/4)/64)*scale);expected_y=f32(-y*scale)
                actual_x=c['design_coordinates'][0]/units;actual_y=-c['design_coordinates'][1]/units
                checks+=1;bad+=(expected_x!=actual_x or expected_y!=actual_y)
    return dict(rectangle_cases=len(data['cases']),rectangle_formula_differences=rectangle_bad,
                polygon_combination_coordinates=checks,combination_differences=bad)


def run(root,exe,header=None):
    root.mkdir(parents=True,exist_ok=True);records=[]
    for units in [16,24,25,26,49,50,51,512,999,1000,1001,1024,2048,16384]:
        path=root/f'polygons-{units}.ttf';order=make_font(path,units);modes={}
        for mode in range(4):
            p=subprocess.run([str(exe),str(path),f'--simulate={mode}','--instructions-dump'],capture_output=True,text=True,timeout=60)
            log=root/f'polygons-{units}-{mode}.txt';log.write_text(p.stdout+p.stderr)
            assert p.returncode in ([0] if mode in [0,2] else [0,6]),(path,mode,p.returncode)
            rows=[list(map(int,x.split('=',1)[1].split(','))) for x in p.stdout.splitlines() if x.startswith('gvar_outline=')]
            assert len(rows)==len(order) and [r[0] for r in rows]==list(range(len(order)))
            modes[str(mode)]={name:decode(row,units) for name,row in zip(order,rows)}
        metric_modes={}
        for mode in range(4):
            p=subprocess.run([str(exe),str(path),f'--simulate={mode}','--metrics-dump'],capture_output=True,text=True,timeout=60)
            (root/f'polygons-{units}-{mode}-metrics.txt').write_text(p.stdout+p.stderr)
            assert p.returncode==0,(path,mode,p.returncode)
            rows=[list(map(int,x.split('=',1)[1].split(','))) for x in p.stdout.splitlines() if x.startswith('reference_metrics=')]
            assert len(rows)==len(order) and [r[0] for r in rows]==list(range(len(order)))
            metric_modes[str(mode)]={name:row[1:] for name,row in zip(order,rows)}
        records.append(dict(font=identity(path),units=units,outlines=modes,metrics=metric_modes))
    data=dict(reference=identity(exe),cases=records)
    (root/'observations.json').write_text(json.dumps(data,indent=2))
    (root/'geometry-analysis.json').write_text(json.dumps(analyse_geometry(data),indent=2))
    if header:emit_metrics(data,header)
    print(f'{len(records)} fonts, {sum(len(c["outlines"]["0"]) for c in records)*4} measured outlines')


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('root',type=Path);parser.add_argument('--exe',required=True,type=Path);parser.add_argument('--metrics-header',type=Path)
    args=parser.parse_args();run(args.root.resolve(),args.exe.resolve(),args.metrics_header)
