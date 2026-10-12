"""Measure instruction influence on normalized outlines with first-party fonts."""
import argparse,array,json,subprocess,struct
from pathlib import Path
from fontTools.ttLib import TTFont,newTable
from fontTools.ttLib.tables.ttProgram import Program
from feature_plan_oracle import identity
def program(assembly):
    p=Program();p.fromAssembly(assembly);return p
def emit_fixture(observations,output):
    """Embed first-party bytecode and Windows point-coordinate bits, not OS fonts."""
    cases=json.loads(observations.read_text())['cases'];blob=bytearray(b'MCTTVM1\0')
    def words(*values):blob.extend(struct.pack('<'+'I'*len(values),*(v&0xffffffff for v in values)))
    words(len(cases))
    for case in cases:
        font=TTFont(Path(case['font']['path']),recalcTimestamp=False)
        glyph=font['glyf']['.notdef'];coords,ends,flags=glyph.getCoordinates(font['glyf'])
        expected=case['outlines'][0]
        assert len(ends)==1 and len(coords)==8 and all(v&1 for v in flags)
        assert expected[:4]==[0,9,1,1] and expected[-1]==5
        words(font['head'].unitsPerEm,len(coords),len(ends),len(font['cvt '].values))
        for x,y in coords:words(x*64,y*64)
        words(*ends);words(*(v*64 for v in font['cvt '].values))
        for code in [font['fpgm'].program.getBytecode(),font['prep'].program.getBytecode(),glyph.program.getBytecode()]:
            words(len(code));blob.extend(code)
        for i in range(8):
            command=expected[8+i*7:15+i*7];assert command[6]==(0 if i==0 else 2)
            words(*command[:2])
    output.parent.mkdir(parents=True,exist_ok=True)
    output.write_text('// Generated from first-party font programs and Windows outline observations.\n'
        'static constexpr u8 tt_interpreter_fixture[]{\n'+''.join('    '+','.join(str(v) for v in blob[i:i+24])+',\n' for i in range(0,len(blob),24))+'};\n')
    print('fixture_bytes',len(blob))

def run(root,source,exe,context=False,fixture=None):
    root.mkdir(parents=True,exist_ok=True);records=[]
    variants={
        'empty':[],
        'shift-x-one':['SVTCA[1]','PUSHB[ ]','0','64','SHPIX[ ]'],
        'shift-x-half':['SVTCA[1]','PUSHB[ ]','0','32','SHPIX[ ]'],
        'shift-y-half':['SVTCA[0]','PUSHB[ ]','0','32','SHPIX[ ]'],
        'round-x':['SVTCA[1]','PUSHB[ ]','0','MDAP[1]'],
        'shift-iup-x':['SVTCA[1]','PUSHB[ ]','0','32','SHPIX[ ]','IUP[1]'],
        'disabled-shift':['SVTCA[1]','PUSHB[ ]','0','64','SHPIX[ ]']}
    if context:
        for name,code in [('ppem',['MPPEM[ ]']),('point-size',['MPS[ ]']),('cvt-zero',['PUSHB[ ]','0','RCVT[ ]']),('coordinate',['PUSHB[ ]','0','GC[0]'])]:
            variants[name]=['SVTCA[1]','PUSHB[ ]','0']+code+['SHPIX[ ]']
        for selector in [1,2,4,8,16,32,64,128,256,512,1024,2048,4095]:
            variants[f'getinfo-{selector}']=['SVTCA[1]','PUSHB[ ]','0','PUSHW[ ]',str(selector),'GETINFO[ ]','SHPIX[ ]']
        for value in [-96,-64,-33,-32,-31,31,32,33,64,96]:
            variants[f'round-{value}']=['SVTCA[1]','PUSHB[ ]','0','PUSHW[ ]',str(value),'ROUND[00]','SHPIX[ ]']
        for kind in ['P','C']:
            for band in range(3):
                for sign,arg in [('positive',8),('negative',7),('inactive',24)]:
                    code=['SVTCA[1]','PUSHW[ ]',str(1000-band*16),'SDB[ ]','PUSHB[ ]',str(arg),'0','1',f'DELTA{kind}{band+1}[ ]']
                    if kind=='C':code+=['PUSHB[ ]','0','0','RCVT[ ]','SHPIX[ ]']
                    variants[f'delta-{kind.lower()}{band+1}-{sign}']=code
        variants['iup-two-anchors']=['SVTCA[1]','PUSHB[ ]','0','32','SHPIX[ ]','PUSHW[ ]','4','-16','SHPIX[ ]','IUP[1]']
        variants['iup-equal-original']=['SVTCA[1]','PUSHB[ ]','5','32','SHPIX[ ]','PUSHW[ ]','6','-16','SHPIX[ ]','IUP[1]']
        variants['iup-equal-right']=['SVTCA[1]','PUSHB[ ]','2','32','SHPIX[ ]','PUSHW[ ]','3','-16','SHPIX[ ]','IUP[1]']
        variants['iup-equal-y']=['SVTCA[0]','PUSHB[ ]','0','32','SHPIX[ ]','PUSHW[ ]','1','-16','SHPIX[ ]','IUP[0]']
        variants['iup-equal-separated']=['SVTCA[0]','PUSHB[ ]','2','32','SHPIX[ ]','PUSHW[ ]','6','-16','SHPIX[ ]','IUP[0]']
        variants['scfs-x']=['SVTCA[1]','PUSHW[ ]','0','672','SCFS[ ]']
        variants['gc-original']=['SVTCA[1]','PUSHB[ ]','0','32','SHPIX[ ]','PUSHB[ ]','1','0','GC[1]','SHPIX[ ]']
        variants['loop-shift']=['SVTCA[1]','PUSHB[ ]','3','SLOOP[ ]','PUSHB[ ]','0','1','2','32','SHPIX[ ]']
    for name,code in variants.items():
        f=TTFont(source,recalcTimestamp=False)
        for tag in ['fvar','gvar']:
            if tag in f:del f[tag]
        f['fpgm']=newTable('fpgm');f['fpgm'].program=program(['PUSHB[ ]','0','FDEF[ ]','ENDF[ ]'])
        f['prep']=newTable('prep');f['prep'].program=program(['PUSHB[ ]','1','1','INSTCTRL[ ]'] if name=='disabled-shift' else ['PUSHB[ ]','0','CALL[ ]'])
        f['cvt ']=newTable('cvt ');f['cvt '].values=array.array('h',[100])
        for g in ['.notdef','A']:f['glyf'][g].program=program(code)
        f['maxp'].maxFunctionDefs=1;f['maxp'].maxStorage=8;f['maxp'].maxStackElements=64
        p=root/(name+'.ttf');f.save(p)
        with TTFont(p,recalcTimestamp=False) as read:read.saveXML(p.with_suffix('.ttx'))
        r=subprocess.run([str(exe.resolve()),str(p.resolve()),'--geometry-dump'],capture_output=True,text=True,timeout=60)
        (root/(name+'.txt')).write_text(r.stdout)
        summary={s.split('=')[0]:int(s.split('=')[1]) for s in r.stdout.splitlines() if '=' in s and not s.startswith('gvar_outline=')}
        outlines=[list(map(int,s.split('=')[1].split(','))) for s in r.stdout.splitlines() if s.startswith('gvar_outline=')]
        records.append(dict(name=name,font=identity(p),assembly=code,returncode=r.returncode,summary=summary,outlines=outlines))
        print(name,r.returncode,summary.get('outline_differences'),outlines[0][8:15] if outlines else [])
    (root/'observations.json').write_text(json.dumps(dict(reference=identity(exe),source=identity(source),cases=records),indent=2)+'\n')
    if fixture:emit_fixture(root/'observations.json',fixture)
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('root',type=Path);p.add_argument('--source',type=Path,required=True);p.add_argument('--exe',type=Path,required=True);p.add_argument('--context',action='store_true');p.add_argument('--fixture',type=Path);a=p.parse_args();run(a.root,a.source,a.exe,a.context,a.fixture)
