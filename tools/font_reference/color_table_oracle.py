"""Independent development comparison of lossless COLR fields through fontTools.

This hashes raw paint semantics, not rendered pixels or resolved variations.
The native probe emits the same canonical stream through independent C++ reads.
"""
import argparse
import json
import struct
from fontTools.ttLib import TTFont


def inspect_font(path,coordinates=None):
    font=TTFont(path)
    table=font['COLR'].table
    bases={record.BaseGlyph:record.Paint for record in table.BaseGlyphList.BaseGlyphPaintRecord}
    clips=getattr(getattr(table,'ClipList',None),'clips',{})
    state={'color_graphs_validated':len(bases),'color_nodes_visited':0,'color_stops_read':0,
           'color_formats':0,'color_maximum_depth':0,'color_has_variations':0}
    digest=14695981039346656037
    instancer=None
    if coordinates is not None and getattr(table,'VarStore',None):
        from fontTools.varLib.varStore import VarStoreInstancer
        instancer=VarStoreInstancer(table.VarStore,font['fvar'].axes,dict(zip((a.axisTag for a in font['fvar'].axes),coordinates)))

    def delta(base,field):
        if instancer is None or base==0xffffffff:return 0
        index=base+field
        if index>0xffffffff:raise ValueError('variation index overflow')
        mapping=getattr(table,'VarIndexMap',None)
        if mapping is not None and mapping.mapping:index=mapping.mapping[min(index,len(mapping.mapping)-1)]
        return instancer[index]

    def packed(data):
        nonlocal digest
        for byte in data:
            digest=((digest^byte)*1099511628211)&0xffffffffffffffff

    def integer(value): packed(struct.pack('<I',int(value)))
    def number(value): packed(struct.pack('<d',float(value)))

    def clip(name):
        box=clips.get(name)
        variable=box is not None and box.Format==2
        integer(box is not None);integer(variable if coordinates is None else False)
        for field,key in enumerate(('xMin','yMin','xMax','yMax')):
            value=getattr(box,key,0)
            if coordinates is not None:
                import math
                if variable:value+=delta(box.VarIndexBase,field)
                value=math.floor(value) if field<2 else math.ceil(value)
            number(value)
        integer(box.VarIndexBase if variable and coordinates is None else 0xffffffff)
        state['color_has_variations']|=int(variable)

    fields={2:['Alpha'],4:['x0','y0','x1','y1','x2','y2'],6:['x0','y0','r0','x1','y1','r1'],
            8:['centerX','centerY','startAngle','endAngle'],12:['xx','yx','xy','yy','dx','dy'],
            14:['dx','dy'],16:['scaleX','scaleY'],18:['scaleX','scaleY','centerX','centerY'],
            20:['scale'],22:['scale','centerX','centerY'],24:['angle'],26:['angle','centerX','centerY'],
            28:['xSkewAngle','ySkewAngle'],30:['xSkewAngle','ySkewAngle','centerX','centerY']}

    def visit(p,depth):
        f=int(p.Format)
        variable=f in (3,5,7,9) or (13<=f<=31 and f%2==1)
        base=f-1 if variable else f
        owner=p.Transform if base==12 else p
        values=[getattr(owner,key) for key in fields.get(base,[])]
        if base==8: values[2:]=[v/180-1 for v in values[2:]]
        elif base in (24,26):values[0]/=180
        elif base in (28,30):values[:2]=[v/180 for v in values[:2]]
        if coordinates is not None:
            for i,key in enumerate(fields.get(base,[])):
                divisor=65536 if base==12 else 16384 if key in ('Alpha','scaleX','scaleY','scale','angle','xSkewAngle','ySkewAngle','startAngle','endAngle') else 1
                if variable:values[i]+=delta(owner.VarIndexBase,i)/divisor
            if base==2:values[0]=min(1,max(0,values[0]))
        integer(f);integer(font.getGlyphID(p.Glyph) if f in (10,11) else 0)
        integer(p.PaletteIndex if base==2 else 0);integer(p.CompositeMode if f==32 else 0)
        integer(p.NumLayers if f==1 else 0);integer(owner.VarIndexBase if variable else 0xffffffff)
        integer(len(values))
        for value in values+[0]*(6-len(values)):number(value)
        state['color_nodes_visited']+=1;state['color_formats']|=1<<(f-1)
        state['color_maximum_depth']=max(state['color_maximum_depth'],depth+1)
        state['color_has_variations']|=int(variable)
        if f==1:
            for child in table.LayerList.Paint[p.FirstLayerIndex:p.FirstLayerIndex+p.NumLayers]:visit(child,depth+1)
        elif 4<=f<=9:
            line=p.ColorLine;integer(line.Extend);integer(len(line.ColorStop))
            stops=[]
            for i,stop in enumerate(line.ColorStop):
                position,alpha=stop.StopOffset,stop.Alpha
                if coordinates is not None:
                    if variable:position+=delta(stop.VarIndexBase,0)/16384;alpha+=delta(stop.VarIndexBase,1)/16384
                    alpha=min(1,max(0,alpha))
                stops.append((position,alpha,stop.PaletteIndex,i if coordinates is not None else stop.VarIndexBase if variable else 0xffffffff))
            if coordinates is not None:stops.sort(key=lambda s:(s[0],s[3]))
            for position,alpha,palette,index in stops:
                number(position);number(alpha);integer(palette);integer(index)
                state['color_stops_read']+=1
        elif f==11:
            clip(p.Glyph);visit(bases[p.Glyph],depth+1)
        elif f==32:
            visit(p.SourcePaint,depth+1);visit(p.BackdropPaint,depth+1)
        elif f>=10:visit(p.Paint,depth+1)

    for name in sorted(bases,key=font.getGlyphID):
        integer(font.getGlyphID(name));clip(name);visit(bases[name],0)
    state['color_value_hash']=digest
    return state


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('font');parser.add_argument('--native-log');parser.add_argument('--output')
    parser.add_argument('--variations',action='store_true')
    args=parser.parse_args()
    if args.variations:
        font=TTFont(args.font);axes=font['COLR'].table.VarStore.VarRegionList.RegionAxisCount
        if axes>2:raise ValueError('QA variation grid supports up to two axes')
        samples=[-1,-.75,-1/3,-.25,0,.125,1/3,.5,.875,1]
        result={i:inspect_font(args.font,[samples[(i//(len(samples)**axis))%len(samples)] for axis in range(axes)])['color_value_hash'] for i in range(len(samples)**axes)}
    else:result=inspect_font(args.font)
    if args.native_log:
        from pathlib import Path
        native={}
        for line in Path(args.native_log).read_text(encoding='utf-8-sig').splitlines():
            if '=' in line:
                key,value=line.split('=',1)
                if args.variations:
                    if key=='variation_sample':sample=int(value)
                    elif key=='variation_value_hash':native[sample]=int(value)
                elif key in result:native[key]=int(value)
        result={'fonttools':result,'native':native,'matches':native==result}
    rendered=json.dumps(result,indent=2)
    if args.output:
        from pathlib import Path
        Path(args.output).write_text(rendered+'\n')
    print(rendered)
    if args.native_log and not result['matches']:raise SystemExit(1)


if __name__=='__main__':main()
