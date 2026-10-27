"""First-party composite/preparation fixtures for integrated bold decoding."""
import argparse,json,struct,subprocess
from pathlib import Path
from fontTools.ttLib import TTFont,newTable
from fontTools.ttLib.tables.ttProgram import Program
from fontTools.pens.ttGlyphPen import TTGlyphPen
from bold_geometry_oracle import make_font,identity
from embed_fixture import write_embedded_array

def run(root,exe,headers):
    root.mkdir(parents=True,exist_ok=True);records=[];blobs={1:bytearray(b'MCTTGL1\0'+struct.pack('<I',12)),3:bytearray(b'MCTTGL1\0'+struct.pack('<I',12))}
    for units in [16,51,1000,1024,2048,16384]:
        for recovered in [False,True]:
            path=root/f'bold-{units}-{int(recovered)}.ttf';make_font(path,units);font=TTFont(path,recalcTimestamp=False)
            additions=[('overlap',[('rectangle',(1,0,0,1,0,0)),('rectangle_reverse',(1,0,0,1,200,0))]),
                ('reflected',[('quadratic',(-1,0,0,1,800,0))]),
                ('nested',[('overlap',(1,0,0,1,-50,25)),('reflected',(1,0,0,1,500,-100))])]
            for name,parts in additions:
                pen=TTGlyphPen(font.getGlyphSet())
                for child,transform in parts:pen.addComponent(child,transform)
                glyph=pen.glyph();font['glyf'][name]=glyph;font.setGlyphOrder(font.getGlyphOrder()+[name] if name not in font.getGlyphOrder() else font.getGlyphOrder());font['hmtx'][name]=(800,0);font['vmtx'][name]=(1000,100)
            if recovered:
                font['prep']=newTable('prep');font['prep'].program=Program();font['prep'].program.fromBytecode(b'\xb0\x00\x2b');font['maxp'].maxStackElements=64
            font.save(path);font=TTFont(path);selected={font.getGlyphID(n) for n in ['quadratic','overlap','reflected','nested']};modes={}
            for mode in [1,3]:
                p=subprocess.run([str(exe),str(path),f'--simulate={mode}','--instructions-dump'],capture_output=True,text=True,timeout=60,creationflags=0x08000000)
                log=root/f'{path.stem}-{mode}.txt';log.write_text(p.stdout+p.stderr);assert p.returncode in [0,6],(path,mode,p.returncode)
                outlines=[list(map(int,s.split('=',1)[1].split(','))) for s in p.stdout.splitlines() if s.startswith('gvar_outline=')]
                outlines=[r for r in outlines if r[0] in selected];assert len(outlines)==4
                data=path.read_bytes();blobs[mode]+=struct.pack('<II',len(data),4)+data
                for row in outlines:blobs[mode]+=struct.pack('<'+'I'*len(row),*row)
                modes[str(mode)]=dict(log=identity(log),outlines=outlines)
            records.append(dict(font=identity(path),units=units,recovered=recovered,modes=modes))
    for mode,path in zip([1,3],headers):
        write_embedded_array(path,'// First-party bold composite outlines; Windows expectations.\n','static constexpr u8 tt_outline_fixture[]',blobs[mode])
    result=dict(reference=identity(exe),cases=records,fixtures=[identity(p) for p in headers]);(root/'observations.json').write_text(json.dumps(result,indent=2));print(len(records),'fonts; 48 glyph outlines per simulation')

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('--exe',type=Path,required=True);p.add_argument('--headers',type=Path,nargs=2,required=True);a=p.parse_args();run(a.root.resolve(),a.exe.resolve(),a.headers)
