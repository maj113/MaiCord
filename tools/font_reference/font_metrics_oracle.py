"""Generate first-party font-metric/line fixtures and capture Windows output."""
from pathlib import Path
import argparse,json,struct,subprocess
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import newTable
from fontTools.ttLib.tables._f_v_a_r import Axis
from fontTools.varLib.builder import buildVarRegionList,buildVarData,buildVarStore
from embed_fixture import write_embedded_array
from gsub_execution_oracle import obj

def make(path,fields):
    b=FontBuilder(fields.get('upem',1000),isTTF=True);b.setupGlyphOrder(['.notdef','a'])
    b.setupCharacterMap({97:'a'});pen=TTGlyphPen(None);pen.moveTo((50,0));pen.lineTo((450,0));pen.lineTo((450,600));pen.lineTo((50,600));pen.closePath()
    b.setupGlyf({'.notdef':TTGlyphPen(None).glyph(),'a':pen.glyph()});b.setupHorizontalMetrics({'.notdef':(600,0),'a':(600,50)})
    b.setupHorizontalHeader(ascent=fields.get('ha',800),descent=fields.get('hd',-200),lineGap=fields.get('hg',100))
    name='MaiCordMetrics'+path.stem;b.setupNameTable(dict(familyName=name,styleName='Regular',uniqueFontIdentifier=name,fullName=name,psName=name))
    b.setupOS2(version=fields.get('version',4),fsSelection=fields.get('flags',64),sTypoAscender=fields.get('ta',700),sTypoDescender=fields.get('td',-300),sTypoLineGap=fields.get('tg',200),usWinAscent=fields.get('wa',900),usWinDescent=fields.get('wd',400),yStrikeoutSize=fields.get('strike_size',50),yStrikeoutPosition=fields.get('strike_pos',300))
    b.setupPost(underlinePosition=fields.get('underline_pos',-100),underlineThickness=fields.get('underline_size',50));b.setupMaxp()
    if fields.get('variable'):
        f=b.font;f['fvar']=newTable('fvar');axis=Axis();axis.axisTag='wght';axis.minValue=100;axis.defaultValue=400;axis.maxValue=800;axis.flags=0;axis.axisNameID=256;f['fvar'].axes=[axis];f['fvar'].instances=[];f['name'].setName('Weight',256,3,1,0x409)
        f['gvar']=newTable('gvar');f['gvar'].version=1;f['gvar'].reserved=0;f['gvar'].variations={g:[] for g in ['.notdef','a']}
        region=buildVarRegionList([{'wght':(0,1,1)}],['wght'])
        f['HVAR']=newTable('HVAR');f['HVAR'].table=obj('HVAR',Version=0x10000,VarStore=buildVarStore(region,[buildVarData([0],[[0],[0]])]),AdvWidthMap=None,LsbMap=None,RsbMap=None)
        tags=sorted(dict(hasc=10,hdsc=20,hlgp=30,hcla=40,hcld=50,undo=60,unds=70,stro=80,strs=90).items())
        values=[obj('MetricsValueRecord',ValueTag=t,VarIdx=i) for i,(t,d) in enumerate(tags)]
        f['MVAR']=newTable('MVAR');f['MVAR'].table=obj('MVAR',Version=0x10000,Reserved=0,ValueRecordSize=8,ValueRecordCount=len(values),ValueRecord=values,VarStore=buildVarStore(region,[buildVarData([0],[[d] for t,d in tags])]))
    b.save(path)

def capture(exe,path,mode,size,weight=0):
    bits=struct.unpack('<I',struct.pack('<f',size))[0]
    args=[str(exe),str(path),'61','default',str(bits),'0','0','en-US',mode]
    if weight:args.append(str(weight))
    result=subprocess.run(args,capture_output=True,text=True,check=True)
    return json.loads(result.stdout)

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('root',type=Path);p.add_argument('--exe',type=Path,required=True);a=p.parse_args();a.root.mkdir(parents=True,exist_ok=True)
    cases=[('default',{}),('typo',{'flags':192}),('large-hhea-gap',{'hg':900}),('negative-hhea-gap',{'hg':-100}),('negative-typo-gap',{'flags':192,'tg':-100}),('zero-win',{'wa':0,'wd':0}),('zero-win-ascent',{'wa':0}),('zero-win-descent',{'wd':0}),('zero-typo',{'flags':192,'ta':0,'td':0}),('positive-descender',{'flags':192,'td':300}),('negative-ascender',{'flags':192,'ta':-700})]
    for version in [0,1,2,3,4,5]:cases.append((f'typo-version-{version}',{'version':version,'flags':192}))
    for upem in [1000,1001,1024,2048]:
        for thickness in [0,-20,1]:cases.append((f'thickness-{upem}-{thickness}',dict(upem=upem,underline_size=thickness,strike_size=thickness)))
    cases += [('variable-win',dict(variable=True)),('variable-typo',dict(variable=True,flags=192)),('variable-win-gap',dict(variable=True,hg=900)),('variable-zero-win',dict(variable=True,wa=0,wd=0)),('variable-zero-thickness',dict(variable=True,underline_size=0,strike_size=0))]
    for variable in [False,True]:
        cases += [(f'strike-zero-{variable}',dict(variable=variable,underline_size=30,strike_size=0)),(f'underline-zero-{variable}',dict(variable=variable,underline_size=0,strike_size=40))]
    rows=[]
    for name,fields in cases:
        path=a.root/(name+'.ttf');make(path,fields)
        for weight in ([400,600] if fields.get('variable') else [0]):
            metric=capture(a.exe,path,'font-metrics',1000,weight)
            lines=[dict(size=size,observed=capture(a.exe,path,'layout',size,weight)) for size in [14,17.5,1000]]
            rows.append(dict(name=name,fields=fields,font=str(path),weight=weight,metrics=metric,layouts=lines))
            print(name,weight,metric['font_metrics'],metric['has_typographic_metrics'])
    (a.root/'observed.json').write_text(json.dumps(rows,indent=2))
    words=lambda *v:struct.pack('<'+'I'*len(v),*[x&0xffffffff for x in v])
    data=b'MCFMET1\0'+words(len(rows))
    for row in rows:
        font=Path(row['font']).read_bytes();m=row['metrics']['font_metrics'];selected=m[:4]+m[6:]
        data+=words(len(font),row['weight'],*selected)+font
        for line in row['layouts']:
            bits=struct.unpack('<I',struct.pack('<f',line['size']))[0]
            data+=words(bits,*line['observed']['layout_lines'][0][3:])
    write_embedded_array(Path(__file__).resolve().parents[2]/'tests/generated/font_metrics_fixture.inc','// Recorded first-party font and default-line metric expectations.\n','constexpr u8 font_metrics_fixture[]',data)
    (a.root/'fixture.bin').write_bytes(data)

if __name__=='__main__':main()
