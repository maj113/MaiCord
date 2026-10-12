"""Offline investigation of captured paragraph point boundaries; not acceptance."""
import json,struct
from pathlib import Path
from bisect import bisect_right
F=struct.Struct('<f');I=struct.Struct('<I')
def f(bits):return F.unpack(I.pack(bits))[0]
def r(value):return F.unpack(F.pack(value))[0]
def add(a,b):return r(a+b)
def sub(a,b):return r(a-b)
def layouts(directory):
 for path in sorted(Path(directory).glob('tamil-*.json')):
  if path.stem.endswith('-verified'):continue
  for n,row in enumerate(json.loads(path.read_text())['layouts']):
   row=dict(row,name=path.stem,index=n)
   row['clusters']=[dict(first=c[0],end=c[1],af=c[2],count=c[3]&0x1fffffff,flags=c[3],left=f(c[4]),width=f(c[5]),style=c[6]) for c in row['clusters']]
   row['runs']=[dict(first=a,count=b,left=f(c),width=f(d),rtl=e&1) for a,b,c,d,e in row['runs']]
   row['advances']=[f(a) for a in row['advances']]
   row['lines']=[dict(first=a,count=b,top=f(c),width=f(d)) for a,b,c,d in row['lines']]
   yield row

def evaluate(row,point,model):
 x,y=map(f,point[:2]);cs=row['clusters'];rs=row['runs'];adv=row['advances']
 line=row['lines'][max(0,bisect_right([l['top'] for l in row['lines']],y)-1)]
 end=line['first']+line['count'];runs=[v for v in rs if line['first']<=v['first']<end]
 if not runs:return 0
 ri=min(bisect_right([v['left'] for v in runs[1:]]+[line['width']],x),len(runs)-1);run=runs[ri]
 while x<0 and line['width']>0 and ri+1<len(runs) and not run['rtl'] and not run['width'] and runs[ri+1]['left']==0:
  ri+=1;run=runs[ri]
 indices=list(range(run['first'],run['first']+run['count']));rtl=run['rtl'];visual=indices[::-1] if rtl else indices
 if model=='legacy':
  rights=[cs[c]['left'] if rtl else add(cs[c]['left'],cs[c]['width']) for c in visual]
  rights[-1]=runs[ri+1]['left'] if ri+1<len(runs) else line['width'];selected=visual[min(bisect_right(rights,x),len(visual)-1)]
 elif model.startswith('walk:'):
  kind,precision,tie=model.split(':')[1:];acc=add(run['left'],run['width']) if rtl else run['left'];selected=indices[-1]
  for c in indices:
   values=[cs[c]['width']] if kind=='cluster' else adv[cs[c]['af']:cs[c]['af']+cs[c]['count']]
   for a in values:acc=r(acc+(-a if rtl else a)) if precision=='float' else acc+(-a if rtl else a)
   matches=(x>=acc if rtl else x<acc) if tie=='physical' else (x>acc if rtl else x<acc) if tie=='strict' else (x>=acc if rtl else x<=acc)
   if matches:selected=c;break
 else:
  # Compare cluster-width and individual-advance reductions independently.
  kind,direction,precision,tie=model.split(':');order=indices if direction=='logical' else visual
  coordinate=sub(add(run['left'],run['width']),x) if direction=='logical' and rtl else sub(x,run['left'])
  acc=0.;rights=[]
  for c in order:
   values=[cs[c]['width']] if kind=='cluster' else adv[cs[c]['af']:cs[c]['af']+cs[c]['count']]
   if rtl and direction=='visual':values=values[::-1]
   for a in values:acc=add(acc,a) if precision=='float' else acc+a
   rights.append(acc)
  selected=order[-1]
  for c,right in zip(order,rights):
   if (coordinate<right if tie=='strict' else coordinate<=right):selected=c;break
 c=cs[selected];left=sub(c['left'],c['width']) if rtl else c['left'];mid=add(left,r(c['width']*.5))
 trailing=not(c['flags']&0x40000000) and (x<mid if rtl else x>=mid)
 return c['end'] if trailing else c['first']

if __name__=='__main__':
 import sys,itertools
 rows=list(layouts(sys.argv[1]));models=['legacy']+['walk:'+':'.join(v) for v in itertools.product(['cluster','glyph'],['float','double'],['strict','inclusive','physical'])]+[':'.join(v) for v in itertools.product(['cluster','glyph'],['logical','visual'],['float','double'],['strict','inclusive'])]
 for model in models:
  diffs=[]
  for row in rows:
   for p in row['points']:
    got=evaluate(row,p,model);expected=p[2]+(p[3] if p[4] else 0)
    if got!=expected:diffs.append((row['name'],row['index'],p[0],got,expected))
  print(model,len(diffs),diffs[:2])
