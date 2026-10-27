"""Generate native contour-direction tests from first-party Windows probes."""
import argparse,json,struct
from pathlib import Path
from fontTools.ttLib import TTFont
from bold_geometry_oracle import make_font,identity,shapes
from bold_orientation_oracle import run as capture
from embed_fixture import write_embedded_array

def make(root,exe):
    root.mkdir(parents=True,exist_ok=True);rect=shapes()['rectangle'][0]
    extra={}
    for x,y in [(600,0),(200,0),(0,0),(0,700),(0,699),(150,250)]:
        moved=[(a+x,b+y) for a,b in reversed(rect)]
        extra[f'mixed_{x}_{y}']=[rect,moved]
        extra[f'mixed_reverse_{x}_{y}']=[list(reversed(rect)),list(reversed(moved))]
    for width in [1,2,3]:
        extra[f'thin_hole_{width}']=[rect,[(200,100),(200+width,100),(200+width,600),(200,600)]]
    path=root/'orientation.ttf';make_font(path,2048,extra)
    font=TTFont(path,recalcTimestamp=False)
    for name in ['duplicate','duplicate_reverse']:
        g=font['glyf'][name];g.coordinates.append(g.coordinates[0]);g.flags.append(g.flags[0]);g.endPtsOfContours[-1]+=1
    font.save(path);glyphs=[i for i,n in enumerate(font.getGlyphOrder()) if len(font['glyf'][n].getCoordinates(font['glyf'])[0])>=3]
    manifest=root/'input.json';manifest.write_text(json.dumps(dict(reference=identity(exe),files=[dict(font=identity(path),analysis=dict(different_glyphs=glyphs))]),indent=2))
    capture(root/'flags',manifest)

def emit(source,output):
    data=json.loads(source.read_text());payload=bytearray(struct.pack('<I',len(data['cases'])));points_total=contours_total=0
    fonts={}
    for row in data['cases']:
        path=Path(row['font']['path']);assert identity(path)==row['font']
        if str(path) not in fonts:fonts[str(path)]=TTFont(path)
        font=fonts[str(path)];glyph=font['glyf'][font.getGlyphOrder()[row['glyph']]];assert not glyph.isComposite()
        points,ends,flags=glyph.getCoordinates(font['glyf']);assert len(ends)==len(row['directions'])
        payload+=struct.pack('<II',len(points),len(ends))
        for end,direction in zip(ends,row['directions']):payload+=struct.pack('<II',end,direction)
        for p,f in zip(points,flags):payload+=struct.pack('<hhI',*p,f&1)
        points_total+=len(points);contours_total+=len(ends)
    write_embedded_array(output,'// Generated from first-party shapes by orientation_first_party.py.\n// Reference observations SHA256 '+identity(source)['sha256']+'\n','static constexpr u8 orientation_fixture[]',payload)
    print(json.dumps(dict(cases=len(data['cases']),points=points_total,contours=contours_total,bytes=len(payload))))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('--exe',type=Path);p.add_argument('--output',required=True,type=Path);a=p.parse_args()
    if a.exe:make(a.root.resolve(),a.exe.resolve())
    emit(a.root/'flags/observations.json',a.output)
