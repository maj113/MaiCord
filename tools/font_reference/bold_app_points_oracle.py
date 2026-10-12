"""Capture native pre-simulation points and Windows bold outlines for app faces.

The optional model analysis is diagnostic. Its differences do not enable bold
in the production renderer or count as passing native outline comparisons.
"""
import argparse,collections,json,re,struct,subprocess
from pathlib import Path
from bold_geometry_oracle import identity
from bold_point_model import expand,f32,nearest

def bits(v):return struct.unpack('<I',struct.pack('<f',v))[0]
def value(v):return struct.unpack('<f',struct.pack('<I',v))[0]

def parse(text):
    points={};base={};reference={}
    for row in text.splitlines():
        for prefix,output in [('native_points=',points),('native_outline=',base),('gvar_outline=',reference)]:
            if row.startswith(prefix):
                data=list(map(int,row[len(prefix):].split(',')));assert data[0] not in output
                output[data[0]]=data;break
    assert points.keys()==base.keys()==reference.keys()
    return points,base,reference

def contours(row):
    glyph,units,count,end_count=row[:4];ends=row[4:4+end_count];data=row[4+end_count:]
    assert len(data)==count*3
    points=[(value(data[i]),value(data[i+1]),data[i+2]&1) for i in range(0,len(data),3)]
    result=[];start=0
    for end in ends:assert start<=end<count;result.append(points[start:end+1]);start=end+1
    assert start==count
    return result

def middle(a,b):return (f32(f32(a[0]+b[0])*.5),f32(f32(a[1]+b[1])*.5),1)
def start_point(c):return c[0] if c[0][2] else c[-1] if c[-1][2] else middle(c[0],c[-1])

def emit(cs,units,origin):
    scale=f32(1/units);third=f32(1/3);two_thirds=f32(1-third);commands=[]
    def normalized(p):return (f32(f32(p[0]+origin)*scale),0.0 if p[1]==0 else f32(-p[1]*scale))
    def command(kind,*points):
        out=[]
        for p in points:out.extend(bits(v) for v in p)
        commands.append(out+[0]*(6-len(out))+[kind])
    for c in cs:
        if len(c)<2:continue
        start=start_point(c);i=1 if c[0][2] else 0;limit=len(c)-(1 if not c[0][2] and c[-1][2] else 0)
        current=normalized(start);command(0,current)
        while i<limit:
            point=normalized(c[i])
            if c[i][2]:command(2,point);current=point;i+=1
            else:
                following=c[i+1] if i+1<limit else start;on=following[2]
                target=normalized(following if on else middle(c[i],following))
                def control(p):return tuple(f32(f32(p[j]*third)+f32(point[j]*two_thirds)) for j in range(2))
                command(3,control(current),control(target),target);current=target;i+=2 if on else 1
        command(5)
    return [v for c in commands for v in c]

def analyse(points,base,reference,grid_fit=True,classify_orientation=False,orientation_font=None):
    counts=collections.Counter();examples=[];maximum_error=0;maximum_glyph=None;different_glyphs=[]
    orientation_cache={}
    for glyph,row in points.items():
        cs=contours(row);units=row[1];original=base[glyph];expected=reference[glyph]
        nonempty=next((c for c in cs if len(c)>1),None)
        origin=nearest((value(original[8])/f32(1/units)-start_point(nonempty)[0])*64)/64 if nonempty else 0
        reconstructed=emit(cs,units,origin);counts['glyphs']+=1;counts['points']+=row[2]
        if reconstructed!=original[8:]:
            counts['base_reconstruction_differences']+=1
            different_glyphs.append(glyph)
            if len(examples)<12:examples.append(dict(glyph=glyph,kind='base',origin=origin))
            continue
        directions=None
        if classify_orientation:
            from bold_orientation_model import classify,classify_glyph
            directions=classify_glyph(orientation_font,orientation_font.getGlyphOrder()[glyph],orientation_cache) if orientation_font is not None else classify(cs)
        widened=expand([[(p[0],p[1]) for p in c] for c in cs],units,grid_fit,directions)
        styled=[[(p[0],p[1],old[2]) for p,old in zip(c,original)] for c,original in zip(widened,cs)]
        predicted=emit(styled,units,origin)
        if len(predicted)!=len(expected[8:]):counts['topology_differences']+=1;different_glyphs.append(glyph)
        elif predicted!=expected[8:]:
            counts['model_glyph_differences']+=1
            different_glyphs.append(glyph)
            coordinate_count=sum(a!=b for a,b in zip(predicted,expected[8:]));counts['model_field_differences']+=coordinate_count
            error=max((abs(value(a)-value(b))*units for i,(a,b) in enumerate(zip(predicted,expected[8:])) if i%7<6),default=0)
            if error>maximum_error:maximum_error=error;maximum_glyph=glyph
            if len(examples)<12:examples.append(dict(glyph=glyph,kind='model',fields=coordinate_count))
        else:counts['model_exact_glyphs']+=1
    return dict(counts=dict(counts),maximum_error_design_units=maximum_error,maximum_error_glyph=maximum_glyph,different_glyphs=different_glyphs,examples=examples)

def analyse_existing(root,classify_orientation=False,component_orientation=False):
    path=root/'observations.json';data=json.loads(path.read_text())
    assert identity(Path(data['reference']['path']))['sha256']==data['reference']['sha256']
    for file in data['files']:
        log=Path(file['logs']['1']['path']);assert identity(log)['sha256']==file['logs']['1']['sha256']
        text=log.read_text();recovered=int(re.search(r'^instruction_prepare_missing_function=(\d+)$',text,re.M)[1])
        font=None
        if component_orientation:
            from fontTools.ttLib import TTFont
            font=TTFont(file['font']['path'])
        file['analysis']=analyse(*parse(text),grid_fit=not recovered,classify_orientation=classify_orientation,orientation_font=font);file['analysis']['grid_fit_from_preparation']=not recovered
        print(file['family'],file['analysis']['counts'],flush=True)
    (root/('component-orientation-analysis.json' if component_orientation else 'orientation-analysis.json' if classify_orientation else 'model-analysis.json')).write_text(json.dumps(data,indent=2))

def run(root,exe,inventory):
    root.mkdir(parents=True,exist_ok=True);files={}
    for line in inventory.read_text().splitlines():
        r=json.loads(line)
        if r['kind']=='face' and r['simulations']==1:files[r['path']]=r['family']
    records=[]
    for index,(path,family) in enumerate(files.items()):
        path=Path(path);modes={};logs={}
        for mode in [0,1]:
            p=subprocess.run([str(exe),str(path),f'--simulate={mode}','--points','--instructions-dump'],capture_output=True,text=True,timeout=60)
            log=root/f'{index}-{path.stem}-{mode}.txt';log.write_text(p.stdout+p.stderr)
            assert p.returncode in ([0] if not mode else [0,6]),(family,mode,p.returncode)
            assert re.search(r'^outline_failures=0$',p.stdout,re.M)
            modes[mode]=parse(p.stdout);logs[mode]=identity(log)
        assert modes[0][0]==modes[1][0] and modes[0][1]==modes[1][1]
        assert modes[0][1]==modes[0][2]
        recovered=int(re.search(r'^instruction_prepare_missing_function=(\d+)$',Path(logs[1]['path']).read_text(),re.M)[1])
        result=analyse(*modes[1],grid_fit=not recovered);result['grid_fit_from_preparation']=not recovered
        records.append(dict(family=family,font=identity(path),logs=logs,analysis=result))
        print(family,result['counts'],flush=True)
    (root/'observations.json').write_text(json.dumps(dict(reference=identity(exe),inventory=identity(inventory),files=records),indent=2))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('--exe',type=Path);p.add_argument('--inventory',type=Path);p.add_argument('--analyse-existing',action='store_true')
    a=p.parse_args()
    if a.analyse_existing:analyse_existing(a.root.resolve())
    else:
        if not a.exe or not a.inventory:p.error('--exe and --inventory are required for capture')
        run(a.root.resolve(),a.exe.resolve(),a.inventory.resolve())
