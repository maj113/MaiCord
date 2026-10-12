"""QA model from first-party probes and pinned scaler inspection, not a provider."""
import argparse,collections,json,math,struct
from pathlib import Path
from fontTools.ttLib import TTFont

def nearest(v):return int(v+.5) if v>=0 else -int(-v+.5)
def cross(a,b):return a[0]*b[1]-a[1]*b[0]
def sub(a,b):return (a[0]-b[0],a[1]-b[1])

def f32(v):return struct.unpack('<f',struct.pack('<f',v))[0]
def rounded_ratio(n,d):
    q=(abs(n)+abs(d)//2)//abs(d)
    return -q if (n<0)!=(d<0) else q

def line_meeting(a0,a1,b0,b1):
    """Measured 26.6 elimination order; rounding happens at every division."""
    ax,ay=sub(a1,a0);bx,by=sub(b1,b0)
    if ay==0:
        if bx==0:return (b0[0],a0[1])
        n=b0[1]-a0[1];d=-by
    elif ax==0:
        if by==0:return (a0[0],b0[1])
        n=b0[0]-a0[0];d=-bx
    elif abs(ax)<abs(ay):
        n=a0[0]-b0[0]+rounded_ratio((b0[1]-a0[1])*ax,ay)
        d=bx-rounded_ratio(by*ax,ay)
    else:
        n=b0[1]-a0[1]-rounded_ratio((b0[0]-a0[0])*ay,ax)
        d=rounded_ratio(bx*ay,ax)-by
    if abs(d)<=16:return tuple((a1[j]+b0[j])//2 for j in range(2))
    return (b0[0]+rounded_ratio(n*bx,d),b0[1]+rounded_ratio(n*by,d))

def expand(contours,units,grid_fit=True,directions=None):
    strength=(units+49)//50;half=strength//2
    x_negative=half*64 if grid_fit else strength*32;x_positive=strength*64-x_negative
    y_positive=((strength-1)//2)*64 if grid_fit else (strength-1)*32;y_negative=(strength-1)*64-y_positive
    if directions is not None:assert len(directions)==len(contours) and all(d in [0,1] for d in directions)
    candidates=[] if directions is not None else [(p[0],-p[1],ci,i) for ci,c in enumerate(contours) if len(set(c))>2 for i,p in enumerate(c)]
    sign=1
    if candidates:
        _,_,ci,i=min(candidates);c=contours[ci];p=c[i];before=(i-1)%len(c);after=(i+1)%len(c)
        while c[before]==p:before=(before-1)%len(c)
        while c[after]==p:after=(after+1)%len(c)
        sign=-1 if cross(sub(p,c[before]),sub(c[after],p))>0 else 1
    def offset(vector):
        x,y=vector;length=math.hypot(x,y)
        nx=nearest(-sign*y*16384/length)//256 if length else 64;ny=nearest(sign*x*16384/length)//256 if length else 0
        if strength==1:return (x_negative+(x_positive if -sign*y>0 else 0),y_negative)
        return (x_negative+(nx*(x_positive if nx>=0 else x_negative)+32)//64,y_negative+(ny*(y_positive if ny>=0 else y_negative)+32)//64)
    output=[]
    for ci,contour in enumerate(contours):
        if directions is not None:sign=-1 if directions[ci] else 1
        if len(contour)<3:output.append(contour[:]);continue
        result=[];i=0;previous=contour[-1]
        while i<len(contour):
            p=contour[i];last=i
            while last+1<len(contour) and contour[last+1]==p:last+=1
            following=contour[last+1] if last+1<len(contour) else contour[0]
            incoming=sub(p,previous);outgoing=sub(following,p)
            a,b=offset(incoming),offset(outgoing)
            def shifted(point,delta):return tuple(nearest(point[j]*64)+delta[j] for j in range(2))
            if a==b:dx,dy=a
            else:
                meeting=line_meeting(shifted(previous,(x_negative,y_negative) if strength==1 else a),shifted(p,a),shifted(p,b),shifted(following,(x_negative,y_negative) if strength==1 else b))
                dx=meeting[0]-nearest(p[0]*64);dy=meeting[1]-nearest(p[1]*64)
            dx=max(0,min(strength*64,dx));dy=max(0,y_negative*2 if dy>(strength-1)*64 else dy)
            result.extend([(p[0]+dx/64,p[1]+dy/64)]*(last-i+1));previous=p;i=last+1
        output.append(result)
    return output

def run(path,output):
    data=json.loads(path.read_text());counts=collections.Counter();examples=[]
    for case in data['cases']:
        font=TTFont(case['font']['path']);units=case['units']
        for name in font.getGlyphOrder():
            glyph=font['glyf'][name]
            points,ends,flags=glyph.getCoordinates(font['glyf'])
            if any(not(f&1) for f in flags):continue
            contours=[];start=0
            for end in ends:contours.append([tuple(p) for p in points[start:end+1]]);start=end+1
            expected=[tuple(round(v*64)/64 for v in c['design_coordinates'][:2]) for c in case['outlines']['1'][name] if c['kind']!=5]
            predicted=[p for contour in expand(contours,units) if len(contour)>1 for p in contour]
            assert len(expected)==len(predicted),(name,len(expected),len(predicted))
            counts['glyphs']+=1;counts['points']+=len(expected)
            for index,(a,b) in enumerate(zip(predicted,expected)):
                if a!=b:
                    counts['differences']+=1
                    if len(examples)<40:examples.append(dict(units=units,name=name,index=index,predicted=a,expected=b))
    result=dict(counts=dict(counts),examples=examples)
    output.write_text(json.dumps(result,indent=2));print(json.dumps(result,indent=2))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('observations',type=Path);p.add_argument('--output',required=True,type=Path);a=p.parse_args();run(a.observations,a.output)
