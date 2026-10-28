"""First-party TrueType positioning probes; Windows is an offline oracle only."""
import argparse,json
from pathlib import Path
from fontTools.ttLib import TTFont
from fontTools.fontBuilder import FontBuilder
from outline_instruction_oracle import program
from feature_plan_oracle import identity
from tt_outline_fixture import run as emit
def push(*values):return ['PUSHW[ ]']+[str(v) for v in values]
def shift(point,value):return push(point,value)+['SHPIX[ ]']
def variants():
    cases={}
    for kind in ['MDRP','MIRP']:
        for flags in range(32):
            code=['SVTCA[1]']+shift(0,32)+push(0)+['SRP0[ ]']
            code+=push(0,1376)+['WCVTP[ ]'] if kind=='MIRP' else []
            code+=push(1,0) if kind=='MIRP' else push(1)
            cases[f'{kind.lower()}-{flags:05b}']=code+[f'{kind}[{flags:05b}]']
        for flag in [0,4,8,12,16,28]:
            for sign,point in [('positive',1),('negative',7)]:
                cases[f'{kind.lower()}-min-{sign}-{flag}']=['SVTCA[1]']+push(1600)+['SMD[ ]']+push(0,1376)+['WCVTP[ ]']+(push(point,0) if kind=='MIRP' else push(point))+[f'{kind}[{flag:05b}]']
        cases[f'{kind.lower()}-single']=['SVTCA[1]']+push(25)+['SSW[ ]']+push(320)+['SSWCI[ ]']+push(0,1376)+['WCVTP[ ]']+(push(1,0) if kind=='MIRP' else push(1))+[f'{kind}[00000]']
    for flag in [0,1]:
        for value in [640,672,708,709,1280,-640]:
            cases[f'miap-{flag}-{value}']=['SVTCA[1]']+push(0,value)+['WCVTP[ ]']+push(0,0)+[f'MIAP[{flag}]']
        for op in ['SHP','SHC','SHZ']:
            cases[f'{op.lower()}-{flag}']=['SVTCA[1]']+shift(0,32)+push(1 if op!='SHC' else 0)+[f'{op}[{flag}]']
            cases[f'{op.lower()}-iup-{flag}']=cases[f'{op.lower()}-{flag}']+['IUP[1]']
        cases[f'msirp-{flag}']=['SVTCA[1]']+shift(0,32)+push(1,1312)+[f'MSIRP[{flag}]']+push(2)+['MDRP[00000]']
        cases[f'md-{flag}']=['SVTCA[1]']+shift(1,32)+push(0,0,1)+[f'MD[{flag}]','SHPIX[ ]']
    for p2 in [4,3]:
        p1=0 if p2==4 else 2
        cases[f'ip-{p1}-{p2}']=['SVTCA[1]']+shift(p1,32)+shift(p2,-16)+push(p1)+['SRP1[ ]']+push(p2)+['SRP2[ ]']+push(1)+['IP[ ]']
    cases['ip-loop']=['SVTCA[1]']+shift(0,32)+shift(4,-16)+push(4)+['SRP2[ ]']+push(3)+['SLOOP[ ]']+push(1,2,3)+['IP[ ]']
    cases['alignrp']=['SVTCA[1]']+shift(0,32)+push(2)+['SLOOP[ ]']+push(1,2)+['ALIGNRP[ ]']
    cases['untouch']=['SVTCA[1]']+shift(0,32)+shift(4,-16)+push(0)+['UTP[ ]','IUP[1]']
    cases['ip-equal-y']=['SVTCA[0]']+shift(0,32)+shift(1,-16)+push(0)+['SRP1[ ]']+push(1)+['SRP2[ ]']+push(2)+['IP[ ]']
    cases['ip-equal-unmoved']=['SVTCA[1]']+push(2)+['SRP1[ ]']+push(3)+['SRP2[ ]']+push(1)+['IP[ ]']
    cases['iup-one-repeat']=['SVTCA[1]']+shift(0,32)+['IUP[1]','IUP[1]']
    cases['iup-negative-delta-tie']=['SVTCA[1]']+shift(0,32)+shift(4,12)+['IUP[1]']
    # A long interval puts a point near a rounding boundary where a prepared
    # 16.16 ratio and a fresh exact division give different coordinates.
    cases['iup-prepared-ratio']=['SVTCA[0]']+push(0)+['MDAP[0]']+shift(4,-64)+['IUP[0]']
    cases['iup-upper-anchor']=['SVTCA[0]']+shift(0,9)+push(4)+['MDAP[0]','IUP[0]']
    cases['iup-upper-anchor-long']=['SVTCA[0]']+shift(0,-39)+push(4)+['MDAP[0]','IUP[0]']
    for span,target,da,db in [(511,383,0,1024),(513,128,0,-1024),(513,384,0,-1024),(100,53,32760,-32760),(100,53,19600,-19568),(100,53,19600,-19569),(100,53,19600,-19567)]:
        cases[f'iup-span-{span}-{target}-{da}-{db}']=['SVTCA[0]']+shift(0,da)+shift(4,db)+['IUP[0]']
    for flag in [0,1]:
        cases[f'twilight-miap-{flag}']=['SVTCA[1]']+push(0,672)+['WCVTP[ ]']+push(0)+['SZP0[ ]']+push(0,0)+[f'MIAP[{flag}]']+push(0)+['SZP2[ ]']+push(0,0)+['GC[1]']+push(1)+['SZP2[ ]','SHPIX[ ]']
    for op in ['SPVTL','SFVTL','SDPVTL']:
        for flag in [0,1]:
            for component in ['x','y']:
                query='GFV[ ]' if op=='SFVTL' else 'GPV[ ]'
                cases[f'vector-{op.lower()}-{flag}-{component}']=['SVTCA[1]']+push(2,0)+[f'{op}[{flag}]']+push(3)+[query]+(['POP[ ]'] if component=='x' else ['SWAP[ ]','POP[ ]'])+['SFVTCA[1]','SHPIX[ ]']
    for x,y in [(16384,16384),(3000,4000),(-3000,4000),(0,0),(1,2),(16383,11)]:
        for op in ['SPVFS','SFVFS']:
            for component in ['x','y']:
                query='GFV[ ]' if op=='SFVFS' else 'GPV[ ]'
                cases[f'stack-{op.lower()}-{x}-{y}-{component}']=['SVTCA[1]']+push(x,y)+[op+'[ ]']+push(3)+[query]+(['POP[ ]'] if component=='x' else ['SWAP[ ]','POP[ ]'])+['SFVTCA[1]','SHPIX[ ]']
    for op in ['SPVTL','SDPVTL']:
        prefix=['SVTCA[1]']+shift(0,32)+push(2,0)+[op+'[0]']
        for component in ['x','y']:
            cases[f'vector-{op.lower()}-moved-{component}']=prefix+push(3)+['GPV[ ]']+(['POP[ ]'] if component=='x' else ['SWAP[ ]','POP[ ]'])+['SFVTCA[1]','SHPIX[ ]']
        for flag in [0,1]:
            cases[f'vector-{op.lower()}-md-{flag}']=prefix+push(3,2,0)+[f'MD[{flag}]','SHPIX[ ]']
            cases[f'vector-{op.lower()}-gc-{flag}']=prefix+push(3,2)+[f'GC[{flag}]','SHPIX[ ]']
        for flag in [0,4,12,28]:
            cases[f'vector-{op.lower()}-mdrp-{flag}']=prefix+push(2)+[f'MDRP[{flag:05b}]']
    for free in [0,1]:
        for projection in [0,1]:
            for amount in [-512,32,1024]:
                cases[f'vector-move-{free}-{projection}-{amount}']=['SVTCA[1]']+push(2,0)+[f'SFVTL[{free}]']+push(4,1)+[f'SPVTL[{projection}]']+push(3,3)+['GC[0]']+push(amount)+['ADD[ ]','SCFS[ ]']
    for x in [-8192,8192]:
        for op in ['GC','MD']:
            for flag in [0,1]:
                cases[f'project-half-{x}-{op.lower()}-{flag}']=['SVTCA[1]']+shift(1,1)+push(x,14189)+['SPVFS[ ]']+(push(3,1) if op=='GC' else push(3,1,0))+[f'{op}[{flag}]','SHPIX[ ]']
    for value in [5436,5437,5440,5572,5573,5574,5575]:
        for op in ['SPVTL','SDPVTL']:
            cases[f'mirp-vector-cut-{op.lower()}-{value}']=['SVTCA[1]']+push(2,0)+[op+'[0]']+push(0,value)+['WCVTP[ ]']+push(2,0)+['MIRP[00100]']
    for amount in [-128,-64,32,64,128,512]:
        for flag in [0,1]:
            cases[f'ip-dual-{amount}-{flag}']=['SVTCA[1]']+shift(0,amount)+push(2,0)+[f'SDPVTL[{flag}]']+push(4)+['SRP2[ ]']+push(1)+['IP[ ]']
            base=['SVTCA[1]']+shift(0,amount)+push(2,0)+[f'SDPVTL[{flag}]']+push(4,0)+['MIRP[11101]']
            cases[f'ip-after-mirp-{amount}-{flag}']=base+push(1)+['IP[ ]']
            cases[f'gc-after-mirp-{amount}-{flag}']=base+push(3,2)+['GC[1]','SHPIX[ ]']
    for amount in [-128,32,512]:
        for flag in [0,1]:
            for offset in [17,-18]:
                cases[f'ip-offset-{amount}-{flag}-{offset}']=['SVTCA[1]']+shift(0,amount)+push(2,0)+[f'SDPVTL[{flag}]']+push(4)+['SRP2[ ]']+push(1)+['IP[ ]']
    for advance in [999,1001,1161]:
        for index in [10,11]:
            cases[f'phantom-odd-{advance}-{index}']=['SVTCA[1]']+push(0,index)+['GC[1]','SCFS[ ]']
    for selector in [0x00,0x11,0x2f,0x40,0x49,0x5f,0x71,0x80,0x9a,0xbf,0x149,-183]:
        for value in [-96,-33,-1,0,1,33,96]:
            cases[f'super-round-{selector}-{value}']=['SVTCA[1]']+push(selector)+['SROUND[ ]']+push(0,value)+['ROUND[00]','SHPIX[ ]']
    for name in ['zero-stack','vertical-empty','disabled-transform','disabled-vertical-transform']:
        cases['policy-'+name]=shift(0,64) if name.startswith('disabled') else []
    return cases
def run(root,source,exe,header):
    fonts=root/'fonts';fonts.mkdir(parents=True,exist_ok=True);records=[]
    for name,code in variants().items():
        font=TTFont(source,recalcTimestamp=False);font['maxp'].maxTwilightPoints=8;font['maxp'].maxStackElements=64
        for glyph in ['.notdef','A']:font['glyf'][glyph].program=program(code)
        if name=='iup-prepared-ratio':
            for glyph in ['.notdef','A']:
                g=font['glyf'][glyph];g.coordinates[0]=(10,168);g.coordinates[2]=(80,615);g.coordinates[4]=(50,1106)
        if name in ['iup-upper-anchor','iup-upper-anchor-long']:
            ys=(828,872,916) if name=='iup-upper-anchor' else (560,783,1057)
            for glyph in ['.notdef','A']:
                g=font['glyf'][glyph];g.coordinates[0]=(10,ys[0]);g.coordinates[2]=(80,ys[1]);g.coordinates[4]=(50,ys[2])
        if name.startswith('iup-span-'):
            _,_,span,target,da,db=name.replace('--','-m').split('-');span,target=int(span),int(target)
            for glyph in ['.notdef','A']:
                g=font['glyf'][glyph];g.coordinates[0]=(10,168);g.coordinates[2]=(80,168+target);g.coordinates[4]=(50,168+span)
        if name.startswith('ip-offset-'):
            offset=int(name.replace('--','-m').split('-')[-1].replace('m','-'))
            for glyph in ['.notdef','A']:
                g=font['glyf'][glyph]
                for i,(x,y) in enumerate(g.coordinates):g.coordinates[i]=(x+offset,y-offset)
                x,y=g.coordinates[1];g.coordinates[1]=(x,y+52)
                advance,lsb=font['hmtx'].metrics[glyph];font['hmtx'].metrics[glyph]=(advance,lsb+offset)
        if name.startswith('phantom-odd-'):
            advance=int(name.split('-')[2])
            for glyph in ['.notdef','A']:font['hmtx'].metrics[glyph]=(advance,-20)
        if name.startswith('policy-'):
            disabled='disabled' in name
            font['prep'].program=program(push(1,1)+['INSTCTRL[ ]'] if disabled else push(511)+['SCANCTRL[ ]']+push(4)+['SCANTYPE[ ]'])
            if not disabled:font['maxp'].maxStackElements=0
            if 'vertical' in name:
                builder=FontBuilder(font=font);builder.setupVerticalMetrics({g:(1000,200) for g in font.getGlyphOrder()});builder.setupVerticalHeader(ascent=800,descent=-200)
            if 'transform' in name:
                for glyph in ['B','C']:
                    for component in font['glyf'][glyph].components:component.transform=[[.75,.125],[.25,1.25]]
        p=fonts/(name+'.ttf');font.save(p);records.append(dict(name=name,font=identity(p),program=code))
    (root/'programs.json').write_text(json.dumps(records,indent=2)+'\n');emit([fonts],exe,root/'oracle',header)
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--root',type=Path,required=True);p.add_argument('--source',type=Path,required=True);p.add_argument('--exe',type=Path,required=True);p.add_argument('--header',type=Path,required=True);a=p.parse_args();run(a.root,a.source,a.exe,a.header)
