"""First-party simple/composite gvar geometry fixtures; offline QA only."""
import argparse,json,subprocess,struct
from pathlib import Path
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import TTFont,newTable
from fontTools.ttLib.tables._f_v_a_r import Axis
from fontTools.ttLib.tables.TupleVariation import TupleVariation
from embed_fixture import write_embedded_array
from feature_plan_oracle import identity

POINTS=[(10,20),(31,20),(80,70),(80,120),(50,180),(-20,130),(-20,70),(-1,30)]
def generate(folder):
    folder.mkdir(parents=True,exist_ok=True);files=[]
    for mode in ['full','sparse','single','two-regions','fraction','transformed','sparse-two','single-two','sparse-fraction','single-fraction']:
        order=['.notdef','A','B','C'];pen=TTGlyphPen(None);pen.moveTo(POINTS[0])
        for p in POINTS[1:]:pen.lineTo(p)
        pen.closePath();simple=pen.glyph();glyphs={'.notdef':simple,'A':simple}
        pen=TTGlyphPen(glyphs);pen.addComponent('A',(.75,0,0,.75,300,100) if mode=='transformed' else (1,0,0,1,300,100));glyphs['B']=pen.glyph()
        pen=TTGlyphPen(glyphs);pen.addComponent('B',(1,0,0,1,-100,200));pen.addComponent('A',(1,0,0,1,30,-200));glyphs['C']=pen.glyph()
        b=FontBuilder(1000,isTTF=True);b.setupGlyphOrder(order);b.setupCharacterMap({ord('a'): 'A',ord('b'):'B',ord('c'):'C'});b.setupGlyf(glyphs);b.setupHorizontalMetrics({g:(1000,0) for g in order});b.setupHorizontalHeader(ascent=800,descent=-200)
        b.setupNameTable({'familyName':'MaiCord gvar '+mode,'styleName':'Regular','uniqueFontIdentifier':'MaiCordGvar-'+mode,'fullName':'MaiCord gvar '+mode,'psName':'MaiCordGvar-'+mode});b.setupOS2(sTypoAscender=800,sTypoDescender=-200,usWinAscent=800,usWinDescent=200);b.setupPost();b.setupMaxp()
        f=b.font;f['fvar']=newTable('fvar');a=Axis();a.axisTag='wght';a.minValue=100;a.defaultValue=400;a.maxValue=900 if 'fraction' in mode else 800;a.flags=0;a.axisNameID=256;f['fvar'].axes=[a];f['fvar'].instances=[];f['name'].setName('Weight',256,3,1,0x409)
        values=list(zip([1,3,5,7,-1,-3,-5,-7],[-3,7,-13,25,25,-13,7,-3]))+[(0,0)]*4
        if mode.startswith('sparse'):values=[values[i] if i in [0,4] else None for i in range(12)]
        if mode.startswith('single'):values=[(3,-7)]+[None]*11
        f['gvar']=newTable('gvar');f['gvar'].version=1;f['gvar'].reserved=0
        f['gvar'].variations={g:[TupleVariation({'wght':(0,1,1)},values)] for g in ['.notdef','A']}
        if mode in ['two-regions','sparse-two','single-two']:
            for g in ['.notdef','A']:f['gvar'].variations[g].append(TupleVariation({'wght':(0,.75,1)},values if mode!='two-regions' else [(3,1)]*8+[(0,0)]*4))
        f['gvar'].variations['B']=[TupleVariation({'wght':(0,1,1)},[(3,-7)]+[(0,0)]*4)]
        f['gvar'].variations['C']=[TupleVariation({'wght':(0,1,1)},[(3,5),(-3,-7)]+[(0,0)]*4)]
        # Variable glyf requires LSB==xMin. Let glyf compilation calculate bounds.
        for g in order:glyphs[g].recalcBounds(f['glyf']);f['hmtx'].metrics[g]=(1000,glyphs[g].xMin)
        f['head'].flags|=2
        p=folder/(mode+'.ttf');f.save(p)
        with TTFont(p,recalcTimestamp=False) as read:read.saveXML(p.with_suffix('.ttx'))
        files.append(p)
    return files
def run(root,exe):
    observations=[]
    for p in generate(root/'fixtures'):
        for weight in [400,600]:
            r=subprocess.run([str(exe.resolve()),str(p.resolve()),f'--gvar{weight}-dump'],capture_output=True,text=True,timeout=60)
            (root/f'{p.stem}-{weight}.txt').write_text(r.stdout)
            summary={s.split('=')[0]:int(s.split('=')[1]) for s in r.stdout.splitlines() if '=' in s and not s.startswith('gvar_outline=')}
            outlines=[list(map(int,s.split('=')[1].split(','))) for s in r.stdout.splitlines() if s.startswith('gvar_outline=')]
            observations.append(dict(font=identity(p),weight=weight,returncode=r.returncode,summary=summary,outlines=outlines))
            print(p.stem,weight,r.returncode,summary.get('outline_differences'),summary.get('outline_failures'))
    (root/'observations.json').write_text(json.dumps(dict(reference=identity(exe),cases=observations),indent=2)+'\n')
def export(root,header):
    data=json.loads((root/'observations.json').read_text());groups={};w=lambda *xs:struct.pack('<'+'I'*len(xs),*xs)
    for c in data['cases']:
        assert c['returncode']==0,c['font']['path'];groups.setdefault(c['font']['path'],[]).append(c)
    parts=[]
    for path,cases in groups.items():
        p=Path(path);assert identity(p)==cases[0]['font'];raw=p.read_bytes();part=w(len(raw),len(cases))+raw
        for c in cases:
            part+=w(c['weight'],len(c['outlines']))
            for record in c['outlines']:part+=w(*record)
        parts.append(part)
    raw=b'MCGVAR1\0'+w(len(parts))+b''.join(parts);(root/'corpus.bin').write_bytes(raw)
    write_embedded_array(header,'// First-party gvar fonts and Windows outline bit expectations.\n','constexpr u8 gvar_outline_fixture[]',raw)
    print('exported',len(parts),'fonts',len(raw),'bytes')
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('root',type=Path);p.add_argument('--exe',type=Path);p.add_argument('--header',type=Path);a=p.parse_args();a.root.mkdir(parents=True,exist_ok=True)
    if a.exe:run(a.root,a.exe)
    if a.header:export(a.root,a.header)
