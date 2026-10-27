"""First-party phantom-advance fixtures and installed DirectWrite observations."""
import argparse,json,struct,subprocess
from pathlib import Path
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import TTFont,newTable
from fontTools.ttLib.tables._f_v_a_r import Axis
from fontTools.ttLib.tables.TupleVariation import TupleVariation
from embed_fixture import write_embedded_array
from feature_plan_oracle import identity

def generate(folder):
    folder.mkdir(parents=True,exist_ok=True);files=[]
    for mode in ['full','sparse','fraction','multiple','empty','inherit','last','scaled','matrix','negative','underflow','tail']:
        order=['.notdef','A','B','C','D'];glyphs={}
        for g in order[:2]:
            pen=TTGlyphPen(None);pen.moveTo((20,0));pen.lineTo((120,0));pen.lineTo((120,300));pen.lineTo((20,300));pen.closePath();glyphs[g]=pen.glyph()
        for g,child in [('B','A'),('C','B')]:
            transform=(.75,.25,.5,.75,30,20) if mode=='matrix' else (.75,0,0,.5,30,20) if mode=='scaled' else (1,0,0,1,30,20)
            pen=TTGlyphPen(glyphs);pen.addComponent(child,transform)
            if g=='C':pen.addComponent('A',(1,0,0,1,200,0))
            glyphs[g]=pen.glyph()
        glyphs['D']=TTGlyphPen(None).glyph()
        if mode in ['inherit','last','scaled','matrix']:
            glyphs['B'].components[0].flags|=0x200;glyphs['C'].components[0].flags|=0x200
            if mode=='last':glyphs['C'].components[1].flags|=0x200;glyphs['C'].components[1].glyphName='.notdef'
        b=FontBuilder(1000,isTTF=True);b.setupGlyphOrder(order);b.setupCharacterMap({ord(c):g for c,g in zip('abcd',order[1:])});b.setupGlyf(glyphs)
        b.setupHorizontalMetrics({g:(600 if mode=='tail' else 600+i*100,0) for i,g in enumerate(order)})
        b.setupHorizontalHeader(ascent=800,descent=-200);b.setupNameTable(dict(familyName='MaiCord phantom '+mode,styleName='Regular',uniqueFontIdentifier='MCphantom-'+mode,fullName='MaiCord phantom '+mode,psName='MCphantom-'+mode))
        b.setupOS2(sTypoAscender=800,sTypoDescender=-200,usWinAscent=800,usWinDescent=200);b.setupPost();b.setupMaxp();f=b.font
        f['fvar']=newTable('fvar');a=Axis();a.axisTag='wght';a.minValue=100;a.defaultValue=400;a.maxValue=900 if mode=='fraction' else 800;a.flags=0;a.axisNameID=256
        f['fvar'].axes=[a];f['fvar'].instances=[];f['name'].setName('Weight',256,3,1,0x409)
        f['gvar']=newTable('gvar');f['gvar'].version=1;f['gvar'].reserved=0;f['gvar'].variations={}
        for i,g in enumerate(order):
            n=0 if g=='D' else len(glyphs[g].components) if glyphs[g].isComposite() else len(glyphs[g].coordinates)
            values=[None if mode=='sparse' else (11,-13)]*n+[(i+1,0),(i*3+4,0),(0,0),(0,0)]
            if mode=='negative':values=[None]*n+[(-i-1,0),(-i*3-4,0),None,None]
            if mode=='underflow':values=[None]*n+[(0,0),(-2000,0),None,None]
            if mode=='empty':values=[None]*n+[(0,0),(0,0),(0,0),(0,0)]
            f['gvar'].variations[g]=[TupleVariation({'wght':(0,1,1)},values)]
            if mode=='multiple':f['gvar'].variations[g].append(TupleVariation({'wght':(0,.75,1)},[None]*n+[(-3,0),(2,0),None,None]))
            glyphs[g].recalcBounds(f['glyf']);f['hmtx'].metrics[g]=(f['hmtx'].metrics[g][0],getattr(glyphs[g],'xMin',0))
        f['head'].flags|=2;f['head'].created=f['head'].modified=2082844800
        path=folder/(mode+'.ttf');f.save(path);files.append(path)
    return files

def run(root,exe):
    observations=[]
    for path in generate(root/'fixtures'):
        cases=[]
        for weight in [400,500,600,700,800]:
            command=[str(exe),str(path),'61','default','1148846080','0','0','en-US','metrics',str(weight)]
            r=subprocess.run(command,capture_output=True,text=True,timeout=10);assert r.returncode==0,(command,r.stdout,r.stderr)
            result=json.loads(r.stdout);assert result['hresult']==0;cases.append(dict(weight=weight,result=result,command=command))
        observations.append(dict(font=identity(path),cases=cases));print(path.stem,[(x['weight'],x['result']['advances']) for x in cases])
    (root/'observations.json').write_text(json.dumps(dict(reference=identity(exe),fonts=observations),indent=2),encoding='utf-8')

def export(root,header,outlines=None):
    data=json.loads((root/'observations.json').read_text(encoding='utf-8'));parts=[];word=lambda *v:struct.pack('<'+'I'*len(v),*v)
    shapes={}
    if outlines:
        observed=json.loads(outlines.read_text(encoding='utf-8'));assert observed['status']=='passed'
        for row in observed['rows']:
            log=Path(row['log']['path']);assert identity(log)==row['log']
            shapes[(row['font']['sha256'],row['weight'])]=[list(map(int,s.split('=')[1].split(','))) for s in log.read_text(encoding='utf-8').splitlines() if s.startswith('gvar_outline=')]
    for font in data['fonts']:
        path=Path(font['font']['path']);assert identity(path)==font['font'];raw=path.read_bytes();part=word(len(raw),len(font['cases']))+raw
        for case in font['cases']:
            values=case['result']['advances'];part+=word(case['weight'],len(values),*values)
            records=shapes.get((font['font']['sha256'],case['weight']),[]);part+=word(len(records))
            for record in records:part+=word(*record)
        parts.append(part)
    raw=b'MCGMET1\0'+word(len(parts))+b''.join(parts)
    write_embedded_array(header,'// First-party HVAR-free fonts and DirectWrite advance expectations.\n','constexpr u8 gvar_metrics_fixture[]',raw)

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('root',type=Path);parser.add_argument('--exe',type=Path);parser.add_argument('--header',type=Path);parser.add_argument('--outlines',type=Path);a=parser.parse_args();a.root.mkdir(parents=True,exist_ok=True)
    if a.exe:run(a.root,a.exe)
    if a.header:export(a.root,a.header,a.outlines)
