"""QA contour classification; no production provider or font-ID exceptions."""
from fractions import Fraction
import math

def extrema(c):
    indices=[]
    for direction in range(4):
        axis=direction//2;other=1-axis;small=direction%2==0;i=0
        for j in range(1,len(c)):
            a,b=c[j],c[i]
            if (a[axis]<b[axis] if small else a[axis]>b[axis]):i=j
            elif a[axis]==b[axis] and (not b[2] or a[2] and (a[other]<b[other] if direction in [0,3] else a[other]>b[other])):i=j
        indices.append(i)
    return indices

def probe_point(c,index,direction):
    p=c[index];axis=direction//2
    if p[2]:return tuple(int(v*2) for v in p[:2])
    prev=c[(index-1)%len(c)];following=c[(index+1)%len(c)]
    a=tuple(Fraction(v*2) if prev[2] else Fraction(v+p[j]) for j,v in enumerate(prev[:2]))
    b=tuple(Fraction(v*2) for v in p[:2]);z=tuple(Fraction(v*2) if following[2] else Fraction(v+p[j]) for j,v in enumerate(following[:2]))
    denominator=a[axis]-2*b[axis]+z[axis]
    if not denominator:return tuple(int(v) for v in b)
    t=(a[axis]-b[axis])/denominator
    values=[(1-t)**2*a[j]+2*t*(1-t)*b[j]+t*t*z[j] for j in range(2)]
    out=[]
    for j,v in enumerate(values):
        if j==axis:out.append(v.numerator//v.denominator if direction%2==0 else -(-v.numerator//v.denominator))
        else:out.append((abs(v.numerator)//v.denominator+1)*(1 if v>=0 else -1))
    return tuple(out)

def order(c,indices):
    a=c[indices[0]];distance=[abs(c[i][0]-a[0])+abs(c[i][1]-a[1]) for i in indices]
    second=2 if distance[2]>distance[3] else 3;other=3 if second==2 else 2
    third=other if distance[other]>distance[1] else 1
    return [0,second,third]

def segments(c):
    middle=lambda a,b:((a[0]+b[0])/2,(a[1]+b[1])/2,1)
    first=c[0] if c[0][2] else c[-1] if c[-1][2] else middle(c[0],c[-1]);p=first
    i=int(bool(c[0][2]));end=len(c)-int(not c[0][2] and c[-1][2]);out=[]
    while i<end:
        q=c[i]
        if q[2]:out.append((p[:2],None,q[:2]));p=q;i+=1
        else:
            next=c[i+1] if i+1<end else first;r=next if next[2] else middle(q,next)
            out.append((p[:2],q[:2],r[:2]));p=r;i+=2 if next[2] else 1
    out.append((p[:2],None,first[:2]));return out

def local_turn(c,index):
    previous=c[(index-1)%len(c)];current=c[index];v=(current[0]-previous[0],current[1]-previous[1])
    for j in range(len(c)):
        following=c[(index+j+1)%len(c)];w=(following[0]-current[0],following[1]-current[1])
        if w==(0,0):continue
        cross=v[0]*w[1]-v[1]*w[0]
        if cross:return 2 if cross>0 else 0
        current=following;v=w
    return 0

def ray(point,direction):
    x,y=point
    if direction<2:return ((x+(1 if direction==0 else -1),y),(-32768 if direction==0 else 32767,y-1))
    return ((x,y+(1 if direction==2 else -1)),(x-1,-32768 if direction==2 else 32767))

def transitions(segs,p,q):
    dx=q[0]-p[0];dy=q[1]-p[1];length=dx*dx+dy*dy;count=0
    lowx,highx=sorted((p[0],q[0]));lowy,highy=sorted((p[1],q[1]))
    for a,control,b in segs:
        ps=[a,b] if control is None else [a,control,b]
        if max(v[0]*2 for v in ps)<lowx or min(v[0]*2 for v in ps)>highx or max(v[1]*2 for v in ps)<lowy or min(v[1]*2 for v in ps)>highy:continue
        side=lambda v:dx*(v[1]*2-p[1])-dy*(v[0]*2-p[0])
        sa,sb=side(a),side(b)
        if control is None:
            if not(sa<=0<sb or sb<=0<sa):continue
            numerator=abs(int(sa))*65536;denominator=abs(int(sb-sa));t=max(1,(numerator+denominator//2)//denominator)
            axis=0 if abs(dx)>abs(dy) else 1;offset=int(a[axis]*2-p[axis])*65536+int((b[axis]-a[axis])*2)*t;extent=(dx if axis==0 else dy)*65536
            if (0<offset<=extent if extent>0 else 0>offset>=extent):count+=1
            continue
        else:
            sc=side(control);aa=sa-2*sc+sb;bb=2*(sc-sa)
            if not aa:
                if not bb:continue
                roots=[-sa/bb]
            else:
                discriminant=bb*bb-4*aa*sa
                if discriminant<=0:continue
                z=-.5*(bb+math.copysign(math.sqrt(discriminant),bb));roots=[z/aa,sa/z] if z else [0]
        for t in roots:
            if t<0 or t>1:continue
            derivative=2*aa*t+bb
            if control is not None and (t==0 and derivative<=0 or t==1 and derivative>=0):continue
            if control is None:x=(1-t)*a[0]+t*b[0];y=(1-t)*a[1]+t*b[1]
            else:x=(1-t)**2*a[0]+2*t*(1-t)*control[0]+t*t*b[0];y=(1-t)**2*a[1]+2*t*(1-t)*control[1]+t*t*b[1]
            projection=(2*x-p[0])*dx+(2*y-p[1])*dy
            if 0<projection<=length:count+=1
    return count

def classify(contours):
    paths=[segments(c) for c in contours];result=[]
    boxes=[(min(p[0]*2 for p in c),min(p[1]*2 for p in c),max(p[0]*2 for p in c),max(p[1]*2 for p in c)) for c in contours]
    for ci,c in enumerate(contours):
        if len(c)<3:result.append(0);continue
        indices=extrema(c);votes=[]
        for d in order(c,indices):
            p,q=ray(probe_point(c,indices[d],d),d)
            lx,hx=sorted((p[0],q[0]));ly,hy=sorted((p[1],q[1]))
            crossings=sum(transitions(path,p,q) for i,path in enumerate(paths) if i!=ci and len(contours[i])>2 and not(boxes[i][2]<lx or boxes[i][0]>hx or boxes[i][3]<ly or boxes[i][1]>hy))
            votes.append(int(((crossings&1)^1)*2==local_turn(c,indices[d])))
            if len(votes)==2 and votes[0]==votes[1]:break
        result.append(votes[-1])
    return result

def classify_glyph(font,name,cache=None):
    """Classify source contours before components are assembled.

    Reflection reverses the retained direction. Singular transforms remain an
    explicit research boundary until they have separate reference probes.
    """
    if cache is None:cache={}
    if name in cache:return cache[name]
    glyph=font['glyf'][name]
    if glyph.isComposite():
        result=[]
        for component in glyph.components:
            matrix=getattr(component,'transform',[[1,0],[0,1]])
            determinant=matrix[0][0]*matrix[1][1]-matrix[0][1]*matrix[1][0]
            assert determinant!=0,(name,'singular transform')
            result.extend(d^int(determinant<0) for d in classify_glyph(font,component.glyphName,cache))
    else:
        points,ends,flags=glyph.getCoordinates(font['glyf']);cs=[];start=0
        for end in ends:cs.append([(*points[i],flags[i]&1) for i in range(start,end+1)]);start=end+1
        result=classify(cs)
    cache[name]=result;return result
